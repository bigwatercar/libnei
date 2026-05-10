#include <gtest/gtest.h>

#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include <neixx/task/sequence_token.h>

namespace nei {

TEST(SequenceTokenTest, CreateReturnsValidToken) {
  const SequenceToken token = SequenceToken::Create();
  EXPECT_TRUE(token.is_valid());
  EXPECT_NE(token.value(), 0u);
}

TEST(SequenceTokenTest, CreateGeneratesUniqueValuesAcrossThreads) {
  constexpr int kThreads = 8;
  constexpr int kPerThread = 512;

  std::unordered_set<std::uint64_t> all_values;
  all_values.reserve(kThreads * kPerThread);
  std::mutex values_lock;

  std::vector<std::thread> workers;
  workers.reserve(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    workers.emplace_back([&]() {
      std::vector<std::uint64_t> local_values;
      local_values.reserve(kPerThread);

      for (int j = 0; j < kPerThread; ++j) {
        const SequenceToken token = SequenceToken::Create();
        local_values.push_back(token.value());
      }

      std::lock_guard<std::mutex> lock(values_lock);
      for (const std::uint64_t value : local_values) {
        all_values.insert(value);
      }
    });
  }

  for (auto& worker : workers) {
    worker.join();
  }

  EXPECT_EQ(all_values.size(), static_cast<std::size_t>(kThreads * kPerThread));
}

}  // namespace nei
