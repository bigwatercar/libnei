#include <neixx/task/time_source.h>

namespace nei {

TimeSource::~TimeSource() = default;

const SystemTimeSource &SystemTimeSource::Instance() {
  static const SystemTimeSource instance;
  return instance;
}

std::chrono::steady_clock::time_point SystemTimeSource::Now() const {
  return std::chrono::steady_clock::now();
}

} // namespace nei