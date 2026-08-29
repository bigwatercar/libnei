#include <neixx/memory/internal_flag.h>

namespace nei {

InternalFlag::InternalFlag() = default;

InternalFlag::~InternalFlag() {
  Invalidate();
}

bool InternalFlag::IsValid() const {
  return valid_.load(std::memory_order_acquire);
}

void InternalFlag::Invalidate() {
  valid_.store(false, std::memory_order_release);
}

} // namespace nei
