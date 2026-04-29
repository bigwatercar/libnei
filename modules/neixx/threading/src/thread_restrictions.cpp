#include <neixx/threading/thread_restrictions.h>

#include <thread>

#include <cstdio>

namespace nei {

namespace {

// Thread-local storage for blocking permission state
// Default is true (blocking allowed) for all threads
thread_local bool g_blocking_allowed = true;
thread_local int g_allow_base_sync_primitives_depth = 0;

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

void ThreadRestrictions::AssertBaseSyncPrimitivesAllowed() {
#ifndef NDEBUG
  if (!BaseSyncPrimitivesAllowed()) {
    std::fprintf(stderr,
                 "ERROR: Base sync primitives disallowed on this thread. "
                 "Use ScopedAllowBaseSyncPrimitives for narrow, intentional waits.\n");
    std::fflush(stderr);
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

bool ThreadRestrictions::BaseSyncPrimitivesAllowed() {
  return g_blocking_allowed || g_allow_base_sync_primitives_depth > 0;
}

int ThreadRestrictions::PushAllowBaseSyncPrimitives() {
  const int previous_depth = g_allow_base_sync_primitives_depth;
  ++g_allow_base_sync_primitives_depth;
  return previous_depth;
}

void ThreadRestrictions::PopAllowBaseSyncPrimitives(int previous_depth) {
  g_allow_base_sync_primitives_depth = previous_depth;
}

void ThreadRestrictions::RestoreBlockingAllowed(bool allowed) {
  g_blocking_allowed = allowed;
}

} // namespace nei
