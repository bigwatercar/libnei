#include <gtest/gtest.h>

#include <neixx/io/io_buffer.h>

namespace nei {

TEST(IOBufferTest, CreateDefaultBuffer) {
  auto buffer = MakeRefCounted<IOBuffer>(1024);

  EXPECT_TRUE(buffer->data() != nullptr);
  EXPECT_EQ(buffer->size(), 1024);
}

TEST(IOBufferTest, BufferCanBeWrittenTo) {
  auto buffer = MakeRefCounted<IOBuffer>(256);
  uint8_t *data = buffer->data();

  for (std::size_t i = 0; i < buffer->size(); ++i) {
    data[i] = static_cast<uint8_t>(i % 256);
  }

  for (std::size_t i = 0; i < buffer->size(); ++i) {
    EXPECT_EQ(data[i], static_cast<uint8_t>(i % 256));
  }
}

TEST(IOBufferTest, ZeroSizeBuffer) {
  auto buffer = MakeRefCounted<IOBuffer>(0);

  EXPECT_EQ(buffer->size(), 0);
  // data() may be nullptr or valid for zero-size allocation
}

TEST(IOBufferTest, LargeBuffer) {
  const std::size_t large_size = 1024 * 1024;  // 1MB
  auto buffer = MakeRefCounted<IOBuffer>(large_size);

  EXPECT_TRUE(buffer->data() != nullptr);
  EXPECT_EQ(buffer->size(), large_size);

  // Spot check some data
  buffer->data()[0] = 42;
  buffer->data()[large_size - 1] = 99;

  EXPECT_EQ(buffer->data()[0], 42);
  EXPECT_EQ(buffer->data()[large_size - 1], 99);
}

TEST(IOBufferTest, PageAlignedAllocation) {
  auto buffer = IOBuffer::CreatePageAligned(4096);

  EXPECT_TRUE(buffer->data() != nullptr);
  EXPECT_EQ(buffer->size(), 4096);
  EXPECT_TRUE(buffer->is_page_aligned());

  // Verify page alignment (address should be divisible by page size, typically 4096)
  uintptr_t addr = reinterpret_cast<uintptr_t>(buffer->data());
  EXPECT_EQ(addr % 4096, 0);
}

TEST(IOBufferTest, MultipleBuffersAreIndependent) {
  auto buffer1 = MakeRefCounted<IOBuffer>(256);
  auto buffer2 = MakeRefCounted<IOBuffer>(256);

  buffer1->data()[0] = 10;
  buffer2->data()[0] = 20;

  EXPECT_EQ(buffer1->data()[0], 10);
  EXPECT_EQ(buffer2->data()[0], 20);
}

TEST(IOBufferTest, BufferRefCounting) {
  scoped_refptr<IOBuffer> buffer1 = MakeRefCounted<IOBuffer>(512);
  uint8_t *original_data = buffer1->data();

  // Create additional reference
  scoped_refptr<IOBuffer> buffer2 = buffer1;

  EXPECT_EQ(buffer2->data(), original_data);

  // Clear first reference
  buffer1 = nullptr;

  // Second reference should still be valid
  EXPECT_TRUE(buffer2->data() != nullptr);
  EXPECT_EQ(buffer2->data(), original_data);
}

TEST(IOBufferTest, PageAlignedLargeAllocation) {
  const std::size_t size = 16 * 4096;  // 16 pages
  auto buffer = IOBuffer::CreatePageAligned(size);

  EXPECT_EQ(buffer->size(), size);
  EXPECT_TRUE(buffer->is_page_aligned());

  uintptr_t addr = reinterpret_cast<uintptr_t>(buffer->data());
  EXPECT_EQ(addr % 4096, 0);
}

}  // namespace nei
