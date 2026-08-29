#include "neixx/io/io_thread.h"

#include <atomic>
#include <memory>

#include <nei/debug/check.h>
#include <neixx/common/at_exit.h>
#include <neixx/synchronization/lock.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/threading/thread.h>

namespace nei {
namespace {

// ---- singleton state (thread-safe) ----
std::atomic<IOThread *> g_io_thread{nullptr};
std::atomic<bool> g_shutdown_registered{false};
Lock g_lock; // protects Start / Shutdown / ResetForTesting transitions

} // namespace

// =============================================================================
// IOThread::Impl
// =============================================================================

class IOThread::Impl {
public:
  Impl() {
    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    thread_ = std::make_unique<Thread>("IOThread");
    thread_->StartWithOptions(opts);
  }

  ~Impl() {
    if (thread_) {
      thread_->Stop();
    }
  }

  void Stop() {
    if (thread_) {
      thread_->Stop();
    }
  }

  scoped_refptr<SingleThreadTaskRunner> task_runner() const {
    return thread_->GetTaskRunner();
  }

private:
  std::unique_ptr<Thread> thread_;
};

// =============================================================================
// IOThread
// =============================================================================

IOThread::IOThread()
    : impl_(std::make_unique<Impl>()) {
}

IOThread::~IOThread() = default;

// static
bool IOThread::Start() {
  // Fast-path: singleton already exists (acquire load is enough — the pointer
  // is never reset outside of ResetForTesting).
  if (g_io_thread.load(std::memory_order_acquire) != nullptr) {
    return true;
  }

  AutoLock lock(g_lock);
  // Double-check under lock: another thread may have raced past the fast-path.
  if (g_io_thread.load(std::memory_order_relaxed) != nullptr) {
    return true;
  }

  auto *instance = new IOThread();
  g_io_thread.store(instance, std::memory_order_release);

  // Register AtExit cleanup — once per process lifetime.
  bool expected_reg = false;
  if (g_shutdown_registered.compare_exchange_strong(
          expected_reg, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    // Register AFTER ThreadPoolInstance (LIFO: IO stops first, pool second).
    // If AtExitManager hasn't been constructed, the callback is a no-op —
    // the caller must Shutdown() manually.
    (void)AtExitManager::RegisterCallback([] { IOThread::Shutdown(); });
  }

  return g_io_thread.load(std::memory_order_relaxed) != nullptr;
}

// static
IOThread *IOThread::Get() {
  return g_io_thread.load(std::memory_order_acquire);
}

// static
void IOThread::Shutdown() {
  // Full teardown: stop the thread, destroy the singleton, and clear the
  // global pointer so a later Start() rebuilds a fresh IO thread.  Idempotent.
  AutoLock lock(g_lock);
  IOThread *snapshot = g_io_thread.load(std::memory_order_relaxed);
  if (snapshot) {
    snapshot->Stop();
    delete snapshot;
    g_io_thread.store(nullptr, std::memory_order_release);
  }
  // Allow Start() to re-register the AtExit cleanup on restart.
  g_shutdown_registered.store(false, std::memory_order_release);
}

// static
void IOThread::ResetForTesting() {
  // Backward-compatible alias.  Shutdown() already performs the full,
  // restartable teardown; this name is kept for existing callers.
  Shutdown();
}

scoped_refptr<SingleThreadTaskRunner> IOThread::task_runner() const {
  return impl_->task_runner();
}

void IOThread::Stop() {
  impl_->Stop();
}

// =============================================================================
// Global convenience
// =============================================================================

scoped_refptr<SingleThreadTaskRunner> GetGlobalIOTaskRunner() {
  auto *io = IOThread::Get();
  return io ? io->task_runner() : nullptr;
}

} // namespace nei
