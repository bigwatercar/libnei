#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include <nei/debug/check.h>
#include <neixx/common/at_exit.h>
#include <neixx/io/io_buffer.h>

namespace nei {
namespace {

TEST(IOBufferTest, IOBufferWithSizeAllocatesWritableStorage) {
  scoped_refptr<IOBufferWithSize> buffer(new IOBufferWithSize(128));
  ASSERT_TRUE(buffer);
  EXPECT_EQ(buffer->size(), 128u);
  ASSERT_NE(buffer->data(), nullptr);

  buffer->data()[0] = static_cast<char>(0x11);
  buffer->data()[127] = static_cast<char>(0x7F);

  EXPECT_EQ(static_cast<std::uint8_t>(buffer->data()[0]), 0x11u);
  EXPECT_EQ(static_cast<std::uint8_t>(buffer->data()[127]), 0x7Fu);
}

TEST(IOBufferTest, WrappedIOBufferUsesExternalStorageWithoutCopy) {
  char external[16] = {};
  external[3] = 'x';

  scoped_refptr<WrappedIOBuffer> wrapped(new WrappedIOBuffer(external));
  ASSERT_TRUE(wrapped);
  ASSERT_EQ(wrapped->data(), external);
  EXPECT_EQ(wrapped->data()[3], 'x');

  wrapped->data()[4] = 'y';
  EXPECT_EQ(external[4], 'y');
}

TEST(IOBufferTest, DrainableIOBufferTracksOffsetAndRemainingBytes) {
  scoped_refptr<IOBufferWithSize> base(new IOBufferWithSize(32));
  ASSERT_TRUE(base);
  for (std::size_t i = 0; i < base->size(); ++i) {
    base->data()[i] = static_cast<char>(i);
  }

  scoped_refptr<DrainableIOBuffer> drainable(
      new DrainableIOBuffer(scoped_refptr<IOBuffer>(base.get()), 16));
  ASSERT_TRUE(drainable);

  EXPECT_EQ(drainable->BytesConsumed(), 0u);
  EXPECT_EQ(drainable->BytesRemaining(), 16u);
  ASSERT_EQ(drainable->data(), base->data());

  drainable->DidConsume(5);
  EXPECT_EQ(drainable->BytesConsumed(), 5u);
  EXPECT_EQ(drainable->BytesRemaining(), 11u);
  ASSERT_EQ(drainable->data(), base->data() + 5);
  EXPECT_EQ(static_cast<std::uint8_t>(drainable->data()[0]), 5u);
}

TEST(IOBufferTest, DrainableIOBufferConsumeAllMovesToEnd) {
  scoped_refptr<IOBufferWithSize> base(new IOBufferWithSize(8));
  ASSERT_TRUE(base);

  scoped_refptr<DrainableIOBuffer> drainable(
      new DrainableIOBuffer(scoped_refptr<IOBuffer>(base.get()), 8));
  ASSERT_TRUE(drainable);

  drainable->DidConsume(8);
  EXPECT_EQ(drainable->BytesConsumed(), 8u);
  EXPECT_EQ(drainable->BytesRemaining(), 0u);
  EXPECT_EQ(drainable->data(), base->data() + 8);
}

TEST(IOBufferTest, IOBufferPoolNormalizesHotBucketSizes) {
  IOBufferPool& pool = IOBufferPool::GetInstance();
  pool.PurgeMemory();

  scoped_refptr<IOBufferWithSize> b1 = pool.AcquireBuffer(1);
  scoped_refptr<IOBufferWithSize> b4k = pool.AcquireBuffer(4096);
  scoped_refptr<IOBufferWithSize> b4k_plus = pool.AcquireBuffer(4097);
  scoped_refptr<IOBufferWithSize> b64k = pool.AcquireBuffer(65536);
  scoped_refptr<IOBufferWithSize> large = pool.AcquireBuffer(70000);

  ASSERT_TRUE(b1);
  ASSERT_TRUE(b4k);
  ASSERT_TRUE(b4k_plus);
  ASSERT_TRUE(b64k);
  ASSERT_TRUE(large);

  EXPECT_EQ(b1->size(), 4096u);
  EXPECT_EQ(b4k->size(), 4096u);
  EXPECT_EQ(b4k_plus->size(), 65536u);
  EXPECT_EQ(b64k->size(), 65536u);
  EXPECT_EQ(large->size(), 70000u);
}

TEST(IOBufferTest, IOBufferPoolReusesReleased4KBuffer) {
  IOBufferPool& pool = IOBufferPool::GetInstance();
  pool.PurgeMemory();
  pool.SetBucketLimitForTesting(4096u, 8u);

  char* first_ptr = nullptr;
  {
    scoped_refptr<IOBufferWithSize> first = pool.AcquireBuffer(1024);
    ASSERT_TRUE(first);
    ASSERT_EQ(first->size(), 4096u);
    first_ptr = first->data();
    ASSERT_NE(first_ptr, nullptr);
  }

  scoped_refptr<IOBufferWithSize> second = pool.AcquireBuffer(2048);
  ASSERT_TRUE(second);
  ASSERT_EQ(second->size(), 4096u);
  ASSERT_NE(second->data(), nullptr);
  EXPECT_EQ(second->data(), first_ptr);
}

#if NEI_DCHECK_IS_ON
TEST(IOBufferTest, DrainableIOBufferOverConsumeTriggersDcheck) {
  scoped_refptr<IOBufferWithSize> base(new IOBufferWithSize(8));
  ASSERT_TRUE(base);
  scoped_refptr<DrainableIOBuffer> drainable(
      new DrainableIOBuffer(scoped_refptr<IOBuffer>(base.get()), 8));
  ASSERT_TRUE(drainable);

  EXPECT_DEATH({ drainable->DidConsume(9); }, "CHECK_LE");
}
#endif

}  // namespace
}  // namespace nei
