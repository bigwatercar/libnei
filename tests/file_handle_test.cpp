#include <gtest/gtest.h>

#include <neixx/io/file_handle.h>
#include <neixx/io/platform_handle.h>

namespace nei {

TEST(FileHandleTest, DefaultConstructorIsInvalid) {
  FileHandle handle;
  EXPECT_FALSE(handle.is_valid());
}

TEST(FileHandleTest, ConstructorWithInvalidHandle) {
  FileHandle handle(kInvalidPlatformHandle, true);
  EXPECT_FALSE(handle.is_valid());
}

TEST(FileHandleTest, OwnershipFlag) {
  // Just test the ownership flag, actual handle validity doesn't matter for this test
  FileHandle owned(kInvalidPlatformHandle, true);
  EXPECT_TRUE(owned.owns_handle());

  FileHandle not_owned(kInvalidPlatformHandle, false);
  EXPECT_FALSE(not_owned.owns_handle());
}

TEST(FileHandleTest, MoveConstructor) {
#if defined(_WIN32)
  FileHandle original(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1)), true);
#else
  FileHandle original(10, true);
#endif
  EXPECT_TRUE(original.is_valid());

  FileHandle moved(std::move(original));
  EXPECT_TRUE(moved.is_valid());
  EXPECT_TRUE(moved.owns_handle());

  // Original should be invalid after move
  EXPECT_FALSE(original.is_valid());
}

TEST(FileHandleTest, MoveAssignment) {
#if defined(_WIN32)
  FileHandle original(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(20)), true);
#else
  FileHandle original(20, true);
#endif
  FileHandle target;

  target = std::move(original);

  EXPECT_TRUE(target.is_valid());
  EXPECT_FALSE(original.is_valid());
}

TEST(FileHandleTest, Release) {
#if defined(_WIN32)
  FileHandle handle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(30)), true);
#else
  FileHandle handle(30, true);
#endif
  EXPECT_TRUE(handle.is_valid());

  PlatformHandle released = handle.Release();
  EXPECT_NE(released, kInvalidPlatformHandle);
  EXPECT_FALSE(handle.is_valid());
}

TEST(FileHandleTest, Reset) {
#if defined(_WIN32)
  FileHandle handle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(40)), true);
  handle.Reset(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(50)), true);
#else
  FileHandle handle(40, true);
  handle.Reset(50, true);
#endif
  EXPECT_TRUE(handle.is_valid());

  handle.Reset(kInvalidPlatformHandle, false);
  EXPECT_FALSE(handle.is_valid());
}

TEST(FileHandleTest, MoveSemantics) {
#if defined(_WIN32)
  FileHandle h1(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(60)), true);
  FileHandle h2(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(70)), false);
#else
  FileHandle h1(60, true);
  FileHandle h2(70, false);
#endif

  h1 = std::move(h2);

  EXPECT_TRUE(h1.is_valid());
  EXPECT_FALSE(h1.owns_handle());
  EXPECT_FALSE(h2.is_valid());
}

TEST(FileHandleTest, MultipleMovesAreValid) {
#if defined(_WIN32)
  FileHandle h1(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(80)), true);
#else
  FileHandle h1(80, true);
#endif
  FileHandle h2(std::move(h1));
  FileHandle h3(std::move(h2));

  EXPECT_FALSE(h1.is_valid());
  EXPECT_FALSE(h2.is_valid());
  EXPECT_TRUE(h3.is_valid());
}

TEST(FileHandleTest, GetOnInvalidHandleIsOkay) {
  FileHandle handle;
  EXPECT_EQ(handle.get(), kInvalidPlatformHandle);
}

}  // namespace nei
