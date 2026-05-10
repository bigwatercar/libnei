#include <neixx/common/time_source.h>

namespace nei {

TimeSource::~TimeSource() = default;

const SystemTimeSource &SystemTimeSource::Instance() {
  static const SystemTimeSource instance;
  return instance;
}

TimeTicks SystemTimeSource::Now() const {
  return TimeTicks::Now();
}

} // namespace nei
