#include <neixx/threading/thread_restrictions.h>

#include <thread>

#include <cstdio>

namespace nei {

namespace {

// Thread-local storage for blocking permission state
// Default is true (blocking allowed) for all threads
thread_local bool g_blocking_allowed = true;

} // namespace

void ThreadRestrictions::AssertBlockingAllowed() {
#ifndef NDEBUG
  if (!g_blocking_allowed) {
    // Log the error using standard fprintf (avoid dependency on logging system)
    std::fprintf(stderr,
                "ERROR: Blocking operation disallowed on this thread. "
                "Use ScopedAllowBlocking if this blocking operation is necessary.\n");
    std::fflush(stderr);
    // Force immediate failure for safety
    std::terminate();
  }
#endif
}

bool ThreadRestrictions::SetBlockingDisallowed() {
  const bool previous = g_blocking_allowed;
  g_blocking_allowed = false;
  return previous;
}

bool ThreadRestrictions::SetBlockingAllowed() {
  const bool previous = g_blocking_allowed;
  g_blocking_allowed = true;
  return previous;
}

bool ThreadRestrictions::BlockingAllowed() {
  return g_blocking_allowed;
}

void ThreadRestrictions::RestoreBlockingAllowed(bool allowed) {
  g_blocking_allowed = allowed;
}

} // namespace nei
