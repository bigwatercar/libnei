#include <neixx/task/sequence_token.h>

#include <atomic>

namespace nei {

SequenceToken SequenceToken::Create() {
  static std::atomic<std::uint64_t> next_id{1};
  return SequenceToken(next_id.fetch_add(1, std::memory_order_relaxed));
}

} // namespace nei
