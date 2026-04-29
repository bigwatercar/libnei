#pragma once

#ifndef NEIXX_THREADING_SEQUENCE_CHECKER_H_
#define NEIXX_THREADING_SEQUENCE_CHECKER_H_

#include <nei/debug/check.h>
#include <nei/macros/nei_export.h>
#include <neixx/threading/platform_thread.h>
#include <neixx/threading/sequence_local_storage_slot.h>

namespace nei {

class SequenceChecker final {
public:
  SequenceChecker()
      : sequence_token_(SequenceLocalStorageMap::GetForCurrentThread())
      , thread_id_(PlatformThread::CurrentId()) {
  }

  SequenceChecker(const SequenceChecker &) = delete;
  SequenceChecker &operator=(const SequenceChecker &) = delete;

  bool CalledOnValidSequence() const {
    SequenceLocalStorageMap *current_sequence_token = SequenceLocalStorageMap::GetForCurrentThread();
    if (sequence_token_ != nullptr) {
      return sequence_token_ == current_sequence_token;
    }

    if (current_sequence_token != nullptr) {
      return false;
    }

    return thread_id_ == PlatformThread::CurrentId();
  }

private:
  SequenceLocalStorageMap *const sequence_token_;
  const PlatformThreadId thread_id_;
};

#if NEI_DCHECK_IS_ON
#define DCHECK_CALLED_ON_VALID_SEQUENCE(checker) DCHECK((checker).CalledOnValidSequence())
#else
#define DCHECK_CALLED_ON_VALID_SEQUENCE(checker) ((void)0)
#endif

} // namespace nei

#endif // NEIXX_THREADING_SEQUENCE_CHECKER_H_