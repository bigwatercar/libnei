#pragma once

#ifndef NEIXX_COMMON_AT_EXIT_H_
#define NEIXX_COMMON_AT_EXIT_H_

#include <functional>
#include <mutex>
#include <vector>

#include <nei/build/nei_export.h>
#include <nei/build/compiler_specific.h>

namespace nei {

// ---------------------------------------------------------------------------
// AtExitManager -- process-global ordered-shutdown facility
// ---------------------------------------------------------------------------
//
// Manages a LIFO stack of cleanup callbacks that are guaranteed to execute in
// reverse registration order at process exit.  This is the single-process
// equivalent of Chromium's `base::AtExitManager`.
//
// Usage (mandatory for any process that registers exit callbacks):
//
//   int main() {
//     nei::AtExitManager at_exit;  // Must be the first stack object in main().
//     // ... register callbacks via AtExitManager::RegisterCallback(...) ...
//     return 0;
//   }  // ~AtExitManager fires ProcessCallbacksNow(), executing all callbacks LIFO.
//
// ---------------------------------------------------------------------------
// ## Linking model constraint (CRITICAL -- READ BEFORE USING AS STATIC LIBRARY)
// ---------------------------------------------------------------------------
//
// `g_top_manager_` and `lock_` are file-scope static members defined in
// `at_exit.cpp`.  In a **shared library (DLL/so) build**, there is exactly one
// copy of these variables in the library's data segment, shared by all
// consumers across the process -- correct by construction.
//
// In a **static library build**, however, each final binary (EXE or DLL) that
// links `neixx` statically receives its own private copy of `g_top_manager_`,
// `lock_`, and the callback `stack_`.  If `neixx` were linked into more than
// one module (e.g. `myapp.exe` AND `mydll.dll`), they would operate on
// independent state and be invisible to each other:
//
//   - The AtExitManager instance created in main() would only capture
//     callbacks registered from code compiled into the EXE.
//   - `RegisterCallback()` called from code inside a DLL would see that DLL's
//     own `g_top_manager_` (nullptr) and silently return false -- the callback
//     is **dropped** and never executes.
//
// This is the same fundamental constraint that Chromium itself operates under:
// in a static build, `base` must be linked **exactly once** into the final
// executable.  If your process loads plugins or components that also consume
// `neixx`, you MUST use a shared-library build (`BUILD_SHARED_LIBS=ON`).
//
// ## Thread safety
//
// `RegisterCallback` is fully thread-safe -- it acquires a mutex internally.
// `ProcessCallbacksNow` executes callbacks *outside* the mutex to prevent
// re-entrant deadlocks (a callback might itself call RegisterCallback or
// attempt to destroy another AtExitManager on a different thread).
//
// ## Singleton rule
//
// At most **one** `AtExitManager` instance may exist at any time.  A second
// construction attempt is caught by DCHECK in debug builds and results in
// immediate process termination (CHECK) in all builds.
//
// ## Relationship with Leaky Singletons
//
// Long-lived process-wide objects (e.g. `IOBufferPool`) should abandon the
// Meyers' Singleton pattern (function-local static) and instead adopt the
// **Leaky Singleton** pattern: allocate via `new`, never delete, and register
// a cleanup callback via `AtExitManager::RegisterCallback` that calls `delete`.
// This guarantees that the object outlives all its users and is only reclaimed
// after every other exit callback has already run.
class NEI_API AtExitManager {
public:
  // The callback type stored on the exit stack.
  // Uses std::function for maximum flexibility; callers may pass lambdas,
  // bind expressions, or function pointers.
  using Callback = std::function<void()>;

  // -----------------------------------------------------------------------
  // Construction / Destruction (RAII root)
  // -----------------------------------------------------------------------

  // Registers *this* as the process-global exit manager.
  //
  // Precondition: no other AtExitManager instance is currently alive.
  // Violation is fatal (CHECK) in all build configurations.
  AtExitManager();

  // Automatically calls ProcessCallbacksNow() to drain the LIFO stack,
  // then unregisters *this* as the active manager.
  ~AtExitManager();

  AtExitManager(const AtExitManager &) = delete;
  AtExitManager &operator=(const AtExitManager &) = delete;

  // -----------------------------------------------------------------------
  // Callback registration
  // -----------------------------------------------------------------------

  // Pushes `callback` onto the process-global LIFO exit stack.
  //
  // Thread-safe.  May be called from any thread at any time after an
  // AtExitManager has been constructed and before ProcessCallbacksNow()
  // begins executing.
  //
  // Returns true if the callback was successfully registered.
  // Returns false if no AtExitManager is currently active.  Common causes:
  //   1. Called before main() creates the AtExitManager.
  //   2. Called from a DLL that links a separate static copy of neixx
  //      (see "Linking model constraint" in the class-level documentation).
  static bool RegisterCallback(Callback callback);

  // -----------------------------------------------------------------------
  // Explicit shutdown
  // -----------------------------------------------------------------------

  // Drains the entire exit stack in LIFO order.
  //
  // Normally invoked automatically by ~AtExitManager().  May also be called
  // explicitly to force early cleanup (e.g. before entering a sandboxed
  // sub-process).  After this call returns the stack is empty; subsequent
  // RegisterCallback() calls will fail until a new AtExitManager is created.
  //
  // Safe to call multiple times; subsequent calls are no-ops.
  static void ProcessCallbacksNow();

private:
  // Points to the single active AtExitManager instance, or nullptr.
  // Read/write is protected by `lock_` in RegisterCallback / ctor / dtor.
  static AtExitManager *g_top_manager_;

  // Guards both `g_top_manager_` and `stack_`.
  static std::mutex lock_;

  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  // The LIFO callback stack.  Owned by the current top manager; emptied by
  // ProcessCallbacksNow() via std::swap under the lock.
  std::vector<Callback> stack_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

} // namespace nei

#endif // NEIXX_COMMON_AT_EXIT_H_
