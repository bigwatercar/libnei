#pragma once

#ifndef NEI_TASK_TIME_SOURCE_H
#define NEI_TASK_TIME_SOURCE_H

#include <chrono>

#include <nei/macros/nei_export.h>

namespace nei {

class NEI_API TimeSource {
public:
  virtual ~TimeSource();

  virtual std::chrono::steady_clock::time_point Now() const = 0;
};

class NEI_API SystemTimeSource final : public TimeSource {
public:
  static const SystemTimeSource &Instance();

  std::chrono::steady_clock::time_point Now() const override;

private:
  SystemTimeSource() = default;
};

} // namespace nei

#endif // NEI_TASK_TIME_SOURCE_H