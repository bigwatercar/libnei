#pragma once

#ifndef NEI_THREADING_THREAD_RESTRICTIONS_H
#define NEI_THREADING_THREAD_RESTRICTIONS_H

#include <nei/macros/nei_export.h>

namespace nei {

// ThreadRestrictions monitors and enforces constraints on blocking operations
// within threads, following Chromium's design pattern.
//
// This is used to prevent accidental blocking calls (like synchronous I/O)
// in performance-sensitive contexts (UI threads, worker threads for quick tasks).
//
// Usage:
//   // Disallow blocking for this scope
//   {
//     ScopedDisallowBlocking disallow;
//     DoSomethingQuick();  // OK
//     SyncIOCall();        // ASSERT! (in debug mode)
//   }
//
//   // Allow blocking in a nested scope
//   {
//     ScopedDisallowBlocking disallow;
//     {
//       ScopedAllowBlocking allow;
//       SyncIOCall();  // OK - temporarily allowed
//     }
//   }

class NEI_API ThreadRestrictions {
public:
  // Checks that blocking is allowed on the current thread.
  // In debug builds, triggers an assertion or log error if blocking is disallowed.
  static void AssertBlockingAllowed();

  // Disables blocking for the current thread.
  // Returns the previous state to allow restoration.
  static bool SetBlockingDisallowed();

  // Enables blocking for the current thread.
  // Returns the previous state to allow restoration.
  static bool SetBlockingAllowed();

  // Returns whether blocking is currently allowed on this thread.
  static bool BlockingAllowed();

private:
  ThreadRestrictions() = delete;
  ~ThreadRestrictions() = delete;
};

// RAII class to disallow blocking operations in a scope.
class NEI_API ScopedDisallowBlocking {
public:
  ScopedDisallowBlocking() noexcept
      : previous_blocking_allowed_(ThreadRestrictions::SetBlockingDisallowed()) {
  }

  ~ScopedDisallowBlocking() noexcept {
    if (previous_blocking_allowed_) {
      ThreadRestrictions::SetBlockingAllowed();
    }
  }

  // Non-copyable and non-movable
  ScopedDisallowBlocking(const ScopedDisallowBlocking &) = delete;
  ScopedDisallowBlocking &operator=(const ScopedDisallowBlocking &) = delete;
  ScopedDisallowBlocking(ScopedDisallowBlocking &&) = delete;
  ScopedDisallowBlocking &operator=(ScopedDisallowBlocking &&) = delete;

private:
  const bool previous_blocking_allowed_;
};

// RAII class to allow blocking operations in a scope (even if disallowed).
class NEI_API ScopedAllowBlocking {
public:
  ScopedAllowBlocking() noexcept
      : previous_blocking_allowed_(ThreadRestrictions::SetBlockingAllowed()) {
  }

  ~ScopedAllowBlocking() noexcept {
    if (!previous_blocking_allowed_) {
      ThreadRestrictions::SetBlockingDisallowed();
    }
  }

  // Non-copyable and non-movable
  ScopedAllowBlocking(const ScopedAllowBlocking &) = delete;
  ScopedAllowBlocking &operator=(const ScopedAllowBlocking &) = delete;
  ScopedAllowBlocking(ScopedAllowBlocking &&) = delete;
  ScopedAllowBlocking &operator=(ScopedAllowBlocking &&) = delete;

private:
  const bool previous_blocking_allowed_;
};

// Convenience macros for assertions and logging
#if defined(NDEBUG)
// In release builds, these are no-ops
#define ASSERT_BLOCKING_ALLOWED() ((void)0)
#else
// In debug builds, these check and report violations
#define ASSERT_BLOCKING_ALLOWED() \
  do { \
    ::nei::ThreadRestrictions::AssertBlockingAllowed(); \
  } while (0)
#endif

} // namespace nei

#endif // NEI_THREADING_THREAD_RESTRICTIONS_H
