#include <neixx/threading/sequence_local_storage_slot.h>

#include <atomic>
#include <unordered_map>

namespace nei {

class SequenceLocalStorageMap::Impl {
public:
  std::unordered_map<std::size_t, std::shared_ptr<void>> values;
};

namespace {

thread_local SequenceLocalStorageMap *g_current_sequence_local_storage_map = nullptr;
std::atomic<std::size_t> g_next_slot_key{1};

} // namespace

SequenceLocalStorageMap::SequenceLocalStorageMap()
    : impl_(std::make_unique<Impl>()) {
}

SequenceLocalStorageMap::~SequenceLocalStorageMap() = default;

SequenceLocalStorageMap::SequenceLocalStorageMap(SequenceLocalStorageMap &&) noexcept = default;

SequenceLocalStorageMap &SequenceLocalStorageMap::operator=(SequenceLocalStorageMap &&) noexcept = default;

bool SequenceLocalStorageMap::HasValue(std::size_t key) const {
  return impl_->values.find(key) != impl_->values.end();
}

std::shared_ptr<void> SequenceLocalStorageMap::GetValue(std::size_t key) const {
  auto it = impl_->values.find(key);
  if (it == impl_->values.end()) {
    return nullptr;
  }
  return it->second;
}

void SequenceLocalStorageMap::SetValue(std::size_t key, std::shared_ptr<void> value) {
  if (value == nullptr) {
    impl_->values.erase(key);
    return;
  }
  impl_->values[key] = std::move(value);
}

void SequenceLocalStorageMap::ClearValue(std::size_t key) {
  impl_->values.erase(key);
}

SequenceLocalStorageMap *SequenceLocalStorageMap::GetForCurrentThread() {
  return g_current_sequence_local_storage_map;
}

SequenceLocalStorageMap *SequenceLocalStorageMap::SwapCurrentForThread(SequenceLocalStorageMap *map) {
  SequenceLocalStorageMap *previous = g_current_sequence_local_storage_map;
  g_current_sequence_local_storage_map = map;
  return previous;
}

std::size_t SequenceLocalStorageMap::AllocateSlotKey() {
  return g_next_slot_key.fetch_add(1, std::memory_order_relaxed);
}

} // namespace nei
