#include <neixx/common/at_exit.h>

#include <nei/debug/check.h>

namespace nei {

// ---------------------------------------------------------------------------
// Static member definitions
// ---------------------------------------------------------------------------

AtExitManager* AtExitManager::g_top_manager_ = nullptr;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::mutex AtExitManager::lock_;

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

AtExitManager::AtExitManager() {
  std::lock_guard<std::mutex> lock(lock_);

  // At most one AtExitManager may exist at any time.  A second construction
  // is always a programmer error  --  DCHECK in debug, CHECK in release  --  and
  // must abort before any damage is done.
  DCHECK_EQ_MSG(g_top_manager_, nullptr,
                "AtExitManager: a second instance was created while one is "
                "already active.  Only one AtExitManager may exist at a time.");

  // In release builds, DCHECK is a no-op.  Use a hard CHECK to guarantee
  // single-instance enforcement in all configurations.
  CHECK_MSG(g_top_manager_ == nullptr,
            "AtExitManager: duplicate instance detected.  Aborting.");

  g_top_manager_ = this;
}

AtExitManager::~AtExitManager() {
  // Step 1: Drain all registered callbacks via the public API.
  // ProcessCallbacksNow() swaps the stack under lock and executes LIFO
  // outside the lock  --  the same deadlock-safe protocol used everywhere.
  ProcessCallbacksNow();

  // Step 2: Unregister *this* so no further callbacks can be registered.
  //
  // There is a narrow window between ProcessCallbacksNow() returning and
  // this lock acquisition where another thread could register a callback
  // on our (now-empty) stack_.  That callback will never execute, which
  // is an acceptable trade-off: the process is shutting down, and the
  // Chromium pattern for single AtExitManager likewise accepts this race.
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (g_top_manager_ == this) {
      g_top_manager_ = nullptr;
    }
  }
}

// ---------------------------------------------------------------------------
// RegisterCallback
// ---------------------------------------------------------------------------

// static
bool AtExitManager::RegisterCallback(Callback callback) {
  if (!callback) {
    return false;  // Null callbacks are silently ignored.
  }

  std::lock_guard<std::mutex> lock(lock_);

  if (g_top_manager_ == nullptr) {
    // No active AtExitManager.  In Chromium this would be a fatal error, but
    // for library code it is safer to return false and let the caller decide.
    return false;
  }

  g_top_manager_->stack_.push_back(std::move(callback));
  return true;
}

// ---------------------------------------------------------------------------
// ProcessCallbacksNow
// ---------------------------------------------------------------------------

// static
void AtExitManager::ProcessCallbacksNow() {
  // Take a snapshot of the current stack under the lock, then release the
  // lock before executing.  This is the same deadlock-prevention strategy
  // used in the destructor.
  std::vector<Callback> local_stack;
  {
    std::lock_guard<std::mutex> lock(lock_);

    if (g_top_manager_ == nullptr) {
      return;  // Nothing to process.
    }

    // Swap: after this, g_top_manager_->stack_ is empty, so new callbacks
    // registered during our execution will go into a fresh stack and will
    // NOT be executed by this call (they will wait for the next drain).
    local_stack.swap(g_top_manager_->stack_);
  }

  // Execute LIFO, outside the lock.
  for (auto it = local_stack.rbegin(); it != local_stack.rend(); ++it) {
    if (*it) {
      (*it)();
    }
  }
}

}  // namespace nei
