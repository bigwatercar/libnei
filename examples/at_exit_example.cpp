// ---------------------------------------------------------------------------
// AtExitManager + Singleton<IOBufferPool, LeakySingletonTraits> Demo
// ---------------------------------------------------------------------------
//
// Demonstrates the complete shutdown lifecycle with the Singleton template:
//
//   1. AtExitManager as the first stack object in main()
//   2. RegisterCallback ordering (LIFO execution at shutdown)
//   3. IOBufferPool::GetInstance() delegates to
//      Singleton<IOBufferPool, LeakySingletonTraits<IOBufferPool>>::GetInstance()
//   4. At exit, LeakySingletonTraits::Delete() -> PurgeMemory()
//      drains cached 4K/64K blocks but keeps the singleton shell alive
//   5. A detached background thread sleeps and then accesses the pool DURING
//      the shutdown window -- proving that the Leaky pattern prevents
//      use-after-free crashes
//
// Expected output (LIFO order during ~AtExitManager):
//   [AtExit] Callback 3 (registered last, runs first)
//   [AtExit] IOBufferPool cleanup -- draining cached 4K/64K blocks
//   [AtExit] Callback 2
//   [AtExit] Callback 1 (registered first, runs last)
//   [Worker] Safely accessed leaky singleton during exit window.
// ---------------------------------------------------------------------------

#include <neixx/common/at_exit.h>
#include <neixx/io/io_buffer.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// ScopedPrint -- RAII helper for visualizing stack unwinding order
// ---------------------------------------------------------------------------
class ScopedPrint {
public:
  explicit ScopedPrint(std::string msg)
      : msg_(std::move(msg)) {
    std::printf("[Ctor] %s\n", msg_.c_str());
  }

  ~ScopedPrint() {
    std::printf("[Dtor]  %s\n", msg_.c_str());
  }

  ScopedPrint(const ScopedPrint &) = delete;
  ScopedPrint &operator=(const ScopedPrint &) = delete;

private:
  std::string msg_;
};

// ---------------------------------------------------------------------------
// BackgroundWorker -- simulates a residual I/O thread that wakes up during
// the process shutdown window.
//
// If IOBufferPool were a traditional singleton (deleted at exit), the
// GetInstance() call below would either:
//   (a) return a dangling pointer -> use-after-free crash, or
//   (b) re-create the pool -> memory leak (orphaned cleanup callback)
//
// With LeakySingletonTraits + Singleton<T>, the pool shell stays alive
// forever.  PurgeMemory() has already freed the cached buffers, but
// the pool object itself is still valid -- GetInstance() returns a live
// reference, and AcquireBuffer() would allocate fresh memory from the OS
// (which will be reclaimed at process exit).
// ---------------------------------------------------------------------------
void BackgroundWorker() {
  // Simulate late wake-up: sleep past the main thread's return,
  // waking up right in the middle of ~AtExitManager callback execution.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  // Critical moment -- if this were a traditional `delete`-based singleton,
  // we would crash here with a null-pointer dereference or use-after-free.
  auto &pool = nei::IOBufferPool::GetInstance();
  std::printf("[Worker] Safely accessed leaky singleton during exit window. "
              "Pool address: %p\n",
              static_cast<void *>(&pool));
}

int main() {
  std::printf("=== AtExitManager + Singleton<IOBufferPool, Leaky> Demo ===\n\n");

  // -----------------------------------------------------------------------
  // Wrap everything in an inner scope so that ~AtExitManager fires BEFORE
  // the final sleep.  This lets the background worker thread finish printing
  // its message before the process exits.
  // -----------------------------------------------------------------------
  {
    // -------------------------------------------------------------------
    // Step 1: AtExitManager at the top of main().
    // -------------------------------------------------------------------
    nei::AtExitManager at_exit;

    // -------------------------------------------------------------------
    // Step 2: Register explicit exit callbacks to demonstrate LIFO ordering.
    // -------------------------------------------------------------------
    std::printf("--- Registering exit callbacks ---\n");

    nei::AtExitManager::RegisterCallback([] { std::printf("[AtExit] Callback 1 (registered first, runs last)\n"); });

    nei::AtExitManager::RegisterCallback([] { std::printf("[AtExit] Callback 2\n"); });

    nei::AtExitManager::RegisterCallback(
        [] { std::printf("[AtExit] IOBufferPool cleanup -- draining cached 4K/64K blocks\n"); });

    nei::AtExitManager::RegisterCallback([] { std::printf("[AtExit] Callback 3 (registered last, runs first)\n"); });

    // -------------------------------------------------------------------
    // Step 3: Access IOBufferPool -- triggers Singleton<>::GetInstance().
    // -------------------------------------------------------------------
    std::printf("\n--- Accessing IOBufferPool (Leaky Singleton) ---\n");

    nei::IOBufferPool &pool = nei::IOBufferPool::GetInstance();
    std::printf("IOBufferPool instance acquired: %p\n", static_cast<void *>(&pool));

    {
      auto buf_4k = pool.AcquireBuffer(4096);
      auto buf_64k = pool.AcquireBuffer(65536);
      std::printf("Acquired 4K buffer:  %p, capacity=%zu\n", static_cast<void *>(buf_4k->data()), buf_4k->capacity());
      std::printf("Acquired 64K buffer: %p, capacity=%zu\n", static_cast<void *>(buf_64k->data()), buf_64k->capacity());
    }

    // -------------------------------------------------------------------
    // Step 4: Launch a detached background thread that outlives main().
    // -------------------------------------------------------------------
    std::printf("\n--- Launching background worker thread ---\n");
    std::thread worker(BackgroundWorker);
    worker.detach();
    std::printf("Background worker detached -- will wake during shutdown.\n");

    // -------------------------------------------------------------------
    // Step 5: Stack objects (destroyed BEFORE AtExitManager).
    // -------------------------------------------------------------------
    std::printf("\n--- Creating stack objects ---\n");
    ScopedPrint scoped_a("Scoped A (bottom of stack)");
    ScopedPrint scoped_b("Scoped B (above Scoped A)");

    std::printf("\n=== main() returning -- ~AtExitManager will now fire ===\n\n");

  } // ~AtExitManager fires here -- LIFO drain of all callbacks

  // -----------------------------------------------------------------------
  // Post-shutdown: give the detached background thread time to wake up,
  // access the (still-valid) Leaky Singleton, and print its message.
  //
  // In a traditional singleton (delete-based), the worker would crash here
  // with a use-after-free.  With LeakySingletonTraits, the pool shell is
  // still alive -- the worker safely accesses GetInstance().
  // -----------------------------------------------------------------------
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::printf("\n=== Process exiting -- OS reclaims all memory ===\n");
  return 0;
}
