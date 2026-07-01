#include <neixx/common/time_source.h>

#include <neixx/common/no_destructor.h>

namespace nei {

TimeSource::~TimeSource() = default;

const SystemTimeSource &SystemTimeSource::Instance() {
  static NoDestructor<SystemTimeSource> instance;
  return *instance;
}

TimeTicks SystemTimeSource::Now() const {
  return TimeTicks::Now();
}

} // namespace nei
