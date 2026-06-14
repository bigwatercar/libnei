#include <neixx/task/thread_pool_instance.h>

#include <utility>

#include <nei/debug/check.h>
#include <neixx/common/at_exit.h>

namespace nei {
namespace {

ThreadPoolInstance* g_instance = nullptr;
bool g_shutdown_registered = false;

}  // namespace

// ---------------------------------------------------------------------------
// ThreadPoolInstance
// ---------------------------------------------------------------------------

ThreadPoolInstance::ThreadPoolInstance(const InitParams& params)
    : pool_(params) {}

ThreadPoolInstance::~ThreadPoolInstance() = default;

// static
ThreadPoolInstance* ThreadPoolInstance::Get() {
  return g_instance;
}

// static
void ThreadPoolInstance::CreateAndStart(const InitParams& params) {
  CHECK_MSG(g_instance == nullptr,
            "ThreadPoolInstance::CreateAndStart() called twice.");

  g_instance = new ThreadPoolInstance(params);

  // Register AtExit cleanup to guarantee ordered shutdown.
  // Registering FIRST ensures ThreadPoolInstance shuts down LAST (LIFO),
  // after all other singletons have already drained their pending work.
  //
  // The cleanup callback drains and joins all worker threads, then
  // intentionally leaks the shell (Leaky Singleton pattern) so any
  // late-arriving background thread can still find a valid pointer.
  if (!g_shutdown_registered) {
    g_shutdown_registered = true;
    bool ok = AtExitManager::RegisterCallback([] {
      if (g_instance) {
        g_instance->pool_.Shutdown();
        // Intentionally do NOT delete g_instance — Leaky Singleton.
        // The OS reclaims the shell memory at process exit.
      }
    });
    CHECK_MSG(ok,
              "ThreadPoolInstance: AtExitManager must be constructed "
              "before CreateAndStart().  Ensure AtExitManager is the "
              "first stack object in main().");
  }
}

// static
void ThreadPoolInstance::CreateAndStartWithDefaultParams() {
  CreateAndStart(InitParams{});
}

// static
void ThreadPoolInstance::Shutdown() {
  // Manual early-shutdown path.  If AtExitManager is active, the AtExit
  // callback will also fire later — the pool's Shutdown() is idempotent
  // (second call is a no-op), so double-drain is harmless.
  if (g_instance == nullptr) {
    return;
  }
  g_instance->pool_.Shutdown();
  // Keep g_instance alive (Leaky) — do NOT delete or nullptr it.
}

scoped_refptr<TaskRunner> ThreadPoolInstance::CreateSequencedTaskRunner(
    const TaskTraits& traits) {
  return pool_.CreateSequencedTaskRunner(traits);
}

// ---------------------------------------------------------------------------
// Global convenience wrappers
// ---------------------------------------------------------------------------

void PostTask(const Location& from_here, OnceClosure task) {
  PostTask(from_here, std::move(task), TaskTraits{});
}

void PostTask(const Location& from_here, OnceClosure task,
              const TaskTraits& traits) {
  ThreadPoolInstance* instance = ThreadPoolInstance::Get();
  DCHECK(instance != nullptr);
  if (instance == nullptr) {
    return;
  }
  scoped_refptr<TaskRunner> runner = instance->CreateSequencedTaskRunner(traits);
  if (runner) {
    runner->PostTask(from_here, std::move(task));
  }
}

scoped_refptr<TaskRunner> CreateSequencedTaskRunner(const TaskTraits& traits) {
  ThreadPoolInstance* instance = ThreadPoolInstance::Get();
  DCHECK(instance != nullptr);
  return instance ? instance->CreateSequencedTaskRunner(traits) : nullptr;
}

}  // namespace nei
