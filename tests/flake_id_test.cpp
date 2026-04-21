#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <thread>
#include <vector>

#include <nei/utils/flake_id.h>

namespace {

constexpr std::uint64_t kSequenceMask = NEI_FLAKE_SEQUENCE_MASK;
constexpr std::uint64_t kThreadTagMask = NEI_FLAKE_THREAD_TAG_MASK;
constexpr std::uint64_t kThreadTagShift = NEI_FLAKE_SEQUENCE_BITS;
constexpr std::uint64_t kTimestampShift = NEI_FLAKE_SEQUENCE_BITS + NEI_FLAKE_THREAD_TAG_BITS;

std::uint64_t ExtractSequence(std::uint64_t id) {
  return id & kSequenceMask;
}

std::uint64_t ExtractThreadTag(std::uint64_t id) {
  return (id >> kThreadTagShift) & kThreadTagMask;
}

std::uint64_t ExtractUnixMs(std::uint64_t id) {
  return ((id >> kTimestampShift) & NEI_FLAKE_TIMESTAMP_MASK) + NEI_FLAKE_EPOCH_MS;
}

} // namespace

TEST(FlakeIdTest, EncodesExpectedBitFields) {
  const std::uint64_t before_ms = nei_flake_unix_ms_now();
  const std::uint64_t id = nei_flake_next_id();
  const std::uint64_t after_ms = nei_flake_unix_ms_now();

  const std::uint64_t extracted_ms = ExtractUnixMs(id);
  const std::uint64_t extracted_tag = ExtractThreadTag(id);
  const std::uint64_t extracted_sequence = ExtractSequence(id);

  EXPECT_GE(extracted_ms, before_ms);
  EXPECT_LE(extracted_ms, after_ms);
  EXPECT_LE(extracted_tag, static_cast<std::uint64_t>(NEI_FLAKE_THREAD_TAG_MASK));
  EXPECT_LE(extracted_sequence, static_cast<std::uint64_t>(NEI_FLAKE_SEQUENCE_MASK));
}

TEST(FlakeIdTest, SingleThreadIdsAreMonotonic) {
  const std::uint64_t first = nei_flake_next_id();
  const std::uint64_t second = nei_flake_next_id();
  const std::uint64_t third = nei_flake_next_id();

  EXPECT_LT(first, second);
  EXPECT_LT(second, third);

  EXPECT_EQ(ExtractThreadTag(first), ExtractThreadTag(second));
  EXPECT_EQ(ExtractThreadTag(second), ExtractThreadTag(third));
}

TEST(FlakeIdTest, MultiThreadedGenerationIsUnique) {
  constexpr std::size_t kThreads = 8;
  constexpr std::size_t kIdsPerThread = 20000;

  std::vector<std::uint64_t> ids(kThreads * kIdsPerThread);
  std::vector<std::thread> workers;
  workers.reserve(kThreads);

  for (std::size_t thread_index = 0; thread_index < kThreads; ++thread_index) {
    workers.emplace_back([&, thread_index]() {
      const std::size_t base = thread_index * kIdsPerThread;
      for (std::size_t i = 0; i < kIdsPerThread; ++i) {
        ids[base + i] = nei_flake_next_id();
      }
    });
  }

  for (std::thread &worker : workers) {
    worker.join();
  }

  std::sort(ids.begin(), ids.end());
  const auto duplicate = std::adjacent_find(ids.begin(), ids.end());
  EXPECT_EQ(duplicate, ids.end());
}