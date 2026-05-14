#include <neixx/task/thread_pool_instance.h>

#include <cassert>
#include <utility>

namespace nei {
namespace {

ThreadPoolInstance* g_instance = nullptr;

}  // namespace

// ---------------------------------------------------------------------------
// ThreadPoolInstance
// ---------------------------------------------------------------------------

ThreadPoolInstance::ThreadPoolInstance()
    : pool_(0) {}  // 0 → uses kDefaultWorkerCount inside ThreadPool::Impl

ThreadPoolInstance::~ThreadPoolInstance() = default;

// static
ThreadPoolInstance* ThreadPoolInstance::Get() {
  return g_instance;
}

// static
void ThreadPoolInstance::CreateAndStartWithDefaultParams() {
  assert(g_instance == nullptr &&
         "ThreadPoolInstance::CreateAndStartWithDefaultParams() called twice.");
  g_instance = new ThreadPoolInstance();
}

// static
void ThreadPoolInstance::Shutdown() {
  if (g_instance == nullptr) {
    return;
  }
  g_instance->pool_.Shutdown();
  delete g_instance;
  g_instance = nullptr;
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
  assert(instance && "PostTask() called before ThreadPoolInstance is initialized.");
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
  assert(instance && "CreateSequencedTaskRunner() called before ThreadPoolInstance is initialized.");
  return instance ? instance->CreateSequencedTaskRunner(traits) : nullptr;
}

}  // namespace nei
