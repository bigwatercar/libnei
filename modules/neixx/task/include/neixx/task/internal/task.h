#pragma once

#ifndef NEIXX_TASK_INTERNAL_TASK_H_
#define NEIXX_TASK_INTERNAL_TASK_H_

#include <cstdint>

#include <neixx/common/location.h>
#include <neixx/common/time.h>
#include <neixx/functional/callback.h>
#include <neixx/task/sequence_token.h>
#include <neixx/task/task_traits.h>

namespace nei {
namespace internal {

struct Task {
  OnceCallback task;
  Location posted_from{"", 0, ""};
  TimeTicks delayed_run_time;
  std::int64_t sequence_num = 0;
  SequenceToken sequence_token;
  TaskTraits traits;

  // Ordering is intentionally based only on delayed_run_time and sequence_num.
  // This keeps queue ordering deterministic and independent from task payload.
  bool operator>(const Task& other) const {
    if (delayed_run_time != other.delayed_run_time) {
      return delayed_run_time > other.delayed_run_time;
    }
    return sequence_num > other.sequence_num;
  }
};

}  // namespace internal
}  // namespace nei

#endif  // NEIXX_TASK_INTERNAL_TASK_H_
