#include "neixx/io/io_thread.h"

#include <memory>

#include <nei/debug/check.h>
#include <neixx/common/at_exit.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/threading/thread.h>

namespace nei {
namespace {

// ---- singleton state ----
IOThread *g_io_thread = nullptr;
bool g_shutdown_registered = false;

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
  if (g_io_thread) {
    return true;
  }

  g_io_thread = new IOThread();

  if (!g_shutdown_registered) {
    g_shutdown_registered = true;
    // Register AFTER ThreadPoolInstance (LIFO: IO stops first, pool
    // second).  If AtExitManager hasn't been constructed, the callback
    // is a no-op — the caller must Shutdown() manually.
    (void)AtExitManager::RegisterCallback([] { IOThread::Shutdown(); });
  }

  return g_io_thread != nullptr;
}

// static
IOThread *IOThread::Get() {
  return g_io_thread;
}

// static
void IOThread::Shutdown() {
  if (g_io_thread) {
    g_io_thread->Stop();
  }
}

// static
void IOThread::ResetForTesting() {
  if (g_io_thread) {
    g_io_thread->Stop();
    delete g_io_thread;
    g_io_thread = nullptr;
  }
  g_shutdown_registered = false;
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
