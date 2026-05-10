#pragma once

#ifndef NEI_COMMON_TIME_SOURCE_H
#define NEI_COMMON_TIME_SOURCE_H

#include <nei/macros/nei_export.h>
#include <neixx/common/time.h>

namespace nei {

class NEI_API TimeSource {
public:
  virtual ~TimeSource();

  // Scheduler-facing time source. We use TimeTicks to keep one monotonic
  // time domain across common/task modules.
  virtual TimeTicks Now() const = 0;
};

class NEI_API SystemTimeSource final : public TimeSource {
public:
  static const SystemTimeSource &Instance();

  TimeTicks Now() const override;

private:
  SystemTimeSource() = default;
};

} // namespace nei

#endif // NEI_COMMON_TIME_SOURCE_H
