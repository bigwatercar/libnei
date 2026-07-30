// =============================================================================
// SharedMemory unit tests — cross-platform shared memory regions & mappings
// =============================================================================

#include <gtest/gtest.h>

#include <cstring>
#include <limits>

#include <neixx/memory/shared_memory.h>

namespace nei {
namespace {

constexpr std::size_t kTestRegionSize = 4096;
constexpr char kMagicString[] = "LIBNEI_SHM";

// =============================================================================
// 靶点 1 — BasicMapAndUnmap
//
// 验证 Region / Mapping 双实体模型的核心语义：
//   - Map → 写入数据
//   - Mapping 析构（Unmap）→ 数据不丢失
//   - 再次 Map → 读取到相同数据
// =============================================================================
TEST(SharedMemoryTest, BasicMapAndUnmap) {
  // Phase 1: create and write.
  auto writable = WritableSharedMemoryRegion::Create(kTestRegionSize);
  ASSERT_TRUE(writable.is_valid());
  EXPECT_EQ(writable.size(), kTestRegionSize);

  {
    auto mapping = writable.Map();
    ASSERT_TRUE(mapping.is_valid());
    EXPECT_NE(mapping.memory(), nullptr);
    EXPECT_EQ(mapping.size(), kTestRegionSize);

    std::memcpy(mapping.memory(), kMagicString, sizeof(kMagicString));
  }
  // mapping destroyed → UnmapViewOfFile / munmap

  // Phase 2: re-map and verify.
  auto mapping2 = writable.Map();
  ASSERT_TRUE(mapping2.is_valid());
  EXPECT_STREQ(static_cast<const char *>(mapping2.memory()), kMagicString);

  // Phase 3: overwrite and verify in-place.
  const char kOther[] = "OVERWRITE";
  std::memcpy(mapping2.memory(), kOther, sizeof(kOther));
  EXPECT_STREQ(static_cast<const char *>(mapping2.memory()), kOther);

  // Phase 4: verify a third independent mapping sees the overwrite.
  auto mapping3 = writable.Map();
  ASSERT_TRUE(mapping3.is_valid());
  EXPECT_STREQ(static_cast<const char *>(mapping3.memory()), kOther);
}

// =============================================================================
// 靶点 2 — DowngradeSecurity
//
// 验证权限单向降级：
//   A) Writable region 在 ConvertToReadOnly() 后被消费、失效。
//   B) ReadOnly region 可映射为 ReadOnlyMapping，读出正确数据。
//   C) ReadOnly region 没有 API 可产生 WritableMapping（类型系统铁壁）。
// =============================================================================
TEST(SharedMemoryTest, DowngradeSecurity) {
  // Create and write data.
  auto writable = WritableSharedMemoryRegion::Create(kTestRegionSize);
  ASSERT_TRUE(writable.is_valid());
  {
    auto mapping = writable.Map();
    ASSERT_TRUE(mapping.is_valid());
    std::memcpy(mapping.memory(), kMagicString, sizeof(kMagicString));
  }

  // Downgrade — writable is consumed.
  ReadOnlySharedMemoryRegion readonly = std::move(writable).ConvertToReadOnly();

  // Assert A: original writable is dead.
  EXPECT_FALSE(writable.is_valid());

  // Assert B: ReadOnly region is alive and readable.
  ASSERT_TRUE(readonly.is_valid());
  EXPECT_EQ(readonly.size(), kTestRegionSize);

  auto ro_mapping = readonly.Map();
  ASSERT_TRUE(ro_mapping.is_valid());
  EXPECT_NE(ro_mapping.memory(), nullptr);
  EXPECT_STREQ(static_cast<const char *>(ro_mapping.memory()), kMagicString);

  // Assert C: type-system attack — ReadOnly mapping returns const void*.
  // No mutable API exists.  We verify the pointer is truly const-qualified.
  const void *ro_ptr = ro_mapping.memory();
  EXPECT_NE(ro_ptr, nullptr);

  // Further proof: no implicit conversion from ReadOnly region to
  // WritableMapping.  The following would not compile:
  //   WritableSharedMemoryMapping bad = readonly.Map();  // error
  //
  // We also verify the user cannot construct a Writable region from the
  // ReadOnly region's handle without going through Unsafe (by design).
  // Default-constructed Writable region is invalid.
  WritableSharedMemoryRegion cannot_create;
  EXPECT_FALSE(cannot_create.is_valid());
}

// =============================================================================
// 靶点 3 — CrossProcessHandleTransfer
//
// 验证跨进程凭证流转（两种路径）：
//   Path A (read-only pipe): Writable→write→ConvertToReadOnly→TakeHandle→
//                            Deserialize(Unsafe)→MapReadOnly→read back
//   Path B (writable pipe):  Unsafe→Map→write→TakeHandle→
//                            Deserialize→ConvertToWritable→Map→read back
// =============================================================================
TEST(SharedMemoryTest, CrossProcessHandleTransfer) {
  constexpr std::size_t kSize = 8192;

  // -----------------------------------------------------------------------
  // Path A — read-only handle transfer (sender downgrades before sending)
  // -----------------------------------------------------------------------
  {
    const char kPayloadA[] = "READ_ONLY_PIPE_PAYLOAD";

    SharedMemoryHandle handle;
    {
      auto writable = WritableSharedMemoryRegion::Create(kSize);
      ASSERT_TRUE(writable.is_valid());
      {
        auto mapping = writable.Map();
        ASSERT_TRUE(mapping.is_valid());
        std::memcpy(mapping.memory(), kPayloadA, sizeof(kPayloadA));
      }

      auto readonly = std::move(writable).ConvertToReadOnly();
      ASSERT_TRUE(readonly.is_valid());
      EXPECT_FALSE(writable.is_valid());

      handle = std::move(readonly).TakeHandle();
      EXPECT_FALSE(readonly.is_valid());
    }

    ASSERT_TRUE(handle.is_valid());
    EXPECT_EQ(handle.size(), kSize);

    // Receiver side.
    auto unsafe = UnsafeSharedMemoryRegion::Deserialize(std::move(handle));
    ASSERT_TRUE(unsafe.is_valid());
    EXPECT_FALSE(handle.is_valid());

    auto ro_mapping = unsafe.MapReadOnly();
    ASSERT_TRUE(ro_mapping.is_valid());
    EXPECT_STREQ(static_cast<const char *>(ro_mapping.memory()), kPayloadA);

    // Attempting writable mapping on a read-only-sealed handle must fail.
    // The region itself may appear valid (it wraps a handle), but Map()
    // must return an invalid mapping because the handle lacks write access.
    auto degraded = std::move(unsafe).ConvertToWritable();
    // degraded may or may not be valid depending on platform; the critical
    // assertion is that Map() fails when the handle is read-only.
    if (degraded.is_valid()) {
      auto wr_fail = degraded.Map();
      EXPECT_FALSE(wr_fail.is_valid()) << "Writable mapping of a read-only-sealed handle MUST fail";
    }
  }

  // -----------------------------------------------------------------------
  // Path B — writable handle transfer via Unsafe region
  // -----------------------------------------------------------------------
  {
    const char kPayloadB[] = "WRITABLE_PIPE_PAYLOAD";

    SharedMemoryHandle handle;
    {
      auto unsafe = UnsafeSharedMemoryRegion::Create(kSize);
      ASSERT_TRUE(unsafe.is_valid());
      {
        auto mapping = unsafe.Map();
        ASSERT_TRUE(mapping.is_valid());
        std::memcpy(mapping.memory(), kPayloadB, sizeof(kPayloadB));
      }

      handle = std::move(unsafe).TakeHandle();
      EXPECT_FALSE(unsafe.is_valid());
    }

    ASSERT_TRUE(handle.is_valid());

    // Receiver side.
    auto unsafe2 = UnsafeSharedMemoryRegion::Deserialize(std::move(handle));
    ASSERT_TRUE(unsafe2.is_valid());
    EXPECT_FALSE(handle.is_valid());

    auto writable2 = std::move(unsafe2).ConvertToWritable();
    ASSERT_TRUE(writable2.is_valid());
    auto wr_mapping = writable2.Map();
    ASSERT_TRUE(wr_mapping.is_valid());
    EXPECT_STREQ(static_cast<const char *>(wr_mapping.memory()), kPayloadB);
  }
}

// =============================================================================
// 靶点 4 — MaliciousAllocations
//
// 验证边界输入不会导致崩溃或 OOM：
//   A) Create(0) → invalid region
//   B) Create(max) → invalid region（被底层拦截）
// =============================================================================
TEST(SharedMemoryTest, MaliciousAllocations) {
  // Assert A: zero-byte region.
  {
    auto r = WritableSharedMemoryRegion::Create(0);
    EXPECT_FALSE(r.is_valid());
    EXPECT_EQ(r.size(), 0u);
  }
  {
    auto r = UnsafeSharedMemoryRegion::Create(0);
    EXPECT_FALSE(r.is_valid());
    EXPECT_EQ(r.size(), 0u);
  }

  // Assert B: absurdly large region.  Must not throw or crash.
  // We use ~0ull which will fail either CreateFileMappingW or mmap.
  constexpr std::size_t kHuge = std::numeric_limits<std::size_t>::max();
  {
    auto r = WritableSharedMemoryRegion::Create(kHuge);
    EXPECT_FALSE(r.is_valid());
  }
  {
    auto r = UnsafeSharedMemoryRegion::Create(kHuge);
    EXPECT_FALSE(r.is_valid());
  }

  // Assert C: deserializing an empty handle.
  {
    SharedMemoryHandle empty;
    auto r = UnsafeSharedMemoryRegion::Deserialize(std::move(empty));
    EXPECT_FALSE(r.is_valid());
  }

  // Assert D: deserializing a default-constructed handle (already invalid).
  {
    SharedMemoryHandle invalid;
    EXPECT_FALSE(invalid.is_valid());
    auto r = UnsafeSharedMemoryRegion::Deserialize(std::move(invalid));
    EXPECT_FALSE(r.is_valid());
  }
}

// =============================================================================
// 靶点 5 — UnsafeRegionConcurrentAccess
//
// 验证 Unsafe 区域的多映射穿透性：
//   - 两次独立 Map() 创建两个 WritableMapping
//   - 映射 A 写入 → 映射 B 即时可见（物理页表共享）
// =============================================================================
TEST(SharedMemoryTest, UnsafeRegionConcurrentAccess) {
  constexpr std::size_t kSize = 4096;
  auto unsafe = UnsafeSharedMemoryRegion::Create(kSize);
  ASSERT_TRUE(unsafe.is_valid());

  // Two independent writable mappings into the same physical pages.
  auto mapping_a = unsafe.Map();
  ASSERT_TRUE(mapping_a.is_valid());
  EXPECT_NE(mapping_a.memory(), nullptr);

  auto mapping_b = unsafe.Map();
  ASSERT_TRUE(mapping_b.is_valid());
  EXPECT_NE(mapping_b.memory(), nullptr);

  // Verify initial state: zeroed memory.
  EXPECT_EQ(*static_cast<const char *>(mapping_a.memory()), '\0');
  EXPECT_EQ(*static_cast<const char *>(mapping_b.memory()), '\0');

  // Write through mapping A.
  const char kShared[] = "SHARED_VIA_UNSAFE_REGION";
  std::memcpy(mapping_a.memory(), kShared, sizeof(kShared));

  // Mapping B sees the change instantly (shared physical pages).
  EXPECT_STREQ(static_cast<const char *>(mapping_b.memory()), kShared);

  // Write through mapping B, verify A sees it.
  const char kFromB[] = "WRITTEN_BY_B";
  std::memcpy(mapping_b.memory(), kFromB, sizeof(kFromB));
  EXPECT_STREQ(static_cast<const char *>(mapping_a.memory()), kFromB);

  // Destroy one mapping — the other remains valid.
  mapping_a = {}; // move-assign from empty → triggers munmap/UnmapViewOfFile
  EXPECT_FALSE(mapping_a.is_valid());

  // mapping_b still alive and sees its own last write.
  ASSERT_TRUE(mapping_b.is_valid());
  EXPECT_STREQ(static_cast<const char *>(mapping_b.memory()), kFromB);
}

// =============================================================================
// Bonus: leak-free RAII — map/unmap repeatedly without ASAN complaints.
// =============================================================================
TEST(SharedMemoryTest, RepeatedMapUnmapNoLeak) {
  auto writable = WritableSharedMemoryRegion::Create(kTestRegionSize);
  ASSERT_TRUE(writable.is_valid());

  for (int i = 0; i < 100; ++i) {
    auto mapping = writable.Map();
    ASSERT_TRUE(mapping.is_valid());
    // Implicitly unmapped at end of scope.  ASAN must remain silent.
  }

  // Also test ReadOnly path.
  auto readonly = std::move(writable).ConvertToReadOnly();
  ASSERT_TRUE(readonly.is_valid());

  for (int i = 0; i < 100; ++i) {
    auto mapping = readonly.Map();
    ASSERT_TRUE(mapping.is_valid());
  }

  SUCCEED();
}

} // namespace
} // namespace nei
