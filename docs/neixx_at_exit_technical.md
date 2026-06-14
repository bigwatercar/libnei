# AtExitManager & Singleton Technical Design

## 1. Document Scope

This document describes the design, API semantics, threading model, deadlock
prevention, Leaky Singleton integration, and static-library multi-module
linking constraints for the `neixx/common` submodule.

Source files referenced:

- `modules/neixx/common/include/neixx/common/at_exit.h` — public API
- `modules/neixx/common/include/neixx/common/singleton.h` — generic singleton container + traits
- `modules/neixx/common/src/at_exit.cpp` — implementation
- `modules/neixx/io/include/neixx/io/io_buffer.h` — IOBufferPool declaration (friend traits)
- `modules/neixx/io/src/io_buffer.cpp` — GetInstance() + LeakySingletonTraits specialization
- `demo/at_exit_demo.cpp` — full integration demo
- `tests/test_main.cpp` — custom GTest main with global AtExitManager

---

## 2. Module Positioning

`AtExitManager` is the **process-global ordered-shutdown root facility**,
equivalent to Chromium's `base::AtExitManager`.

| Capability | Description |
|---|---|
| **LIFO callback stack** | Last-registered callback executes first, matching C++ destructor semantics |
| **RAII lifecycle** | Constructed at the top of `main()`, destructor auto-fires all callbacks |
| **Thread-safe registration** | Any thread may call `RegisterCallback` at any time |
| **Deadlock-proof execution** | Callbacks execute outside the internal mutex (swap-under-lock) |
| **Explicit mid-life drain** | `ProcessCallbacksNow()` empties the stack without destroying the manager |

`Singleton<T, Traits>` is the **generic singleton container** that decouples
allocation strategy, destruction timing, and thread safety via the Traits policy
pattern. It fully replaces ad-hoc Meyers' Singletons and manual DCL patterns.

---

## 3. AtExitManager API Reference

### 3.1 Class Declaration

```cpp
class NEI_API AtExitManager {
public:
    using Callback = std::function<void()>;

    AtExitManager();                              // Register as global manager (at most one)
    ~AtExitManager();                             // Auto-calls ProcessCallbacksNow()

    AtExitManager(const AtExitManager&) = delete;
    AtExitManager& operator=(const AtExitManager&) = delete;

    static bool RegisterCallback(Callback cb);    // Thread-safe, returns false if no manager
    static void ProcessCallbacksNow();            // Explicit LIFO drain
};
```

### 3.2 Basic Usage

```cpp
int main() {
    nei::AtExitManager at_exit;  // MUST be the first stack object in main()

    AtExitManager::RegisterCallback([] { CleanupA(); });
    AtExitManager::RegisterCallback([] { CleanupB(); });
    // On return: CleanupB runs first (LIFO), then CleanupA

    return 0;
}
```

### 3.3 RegisterCallback Return Value

| Return | Meaning | Common causes |
|--------|---------|--------------|
| `true` | Callback pushed onto LIFO stack | Normal path |
| `false` | No active AtExitManager | (1) Called before `main()`; (2) DLL holds a separate static copy of neixx (see Section 5) |

---

## 4. AtExitManager Architecture

### 4.1 Core Data Structure

```
+-- AtExitManager (singleton instance) ------------------------------------+
|                                                                          |
|  static AtExitManager* g_top_manager_  <- process-unique pointer          |
|  static std::mutex     lock_           <- guards all operations below     |
|                                                                          |
|  std::vector<Callback> stack_          <- LIFO callback stack             |
|                                                                          |
|  RegisterCallback(cb):                                                   |
|    lock_ -> stack_.push_back(cb) -> unlock                                |
|                                                                          |
|  ProcessCallbacksNow():                                                   |
|    lock_ -> local.swap(stack_) -> unlock                                  |
|    for (auto& cb : reverse(local)) cb();  <- executes OUTSIDE the lock!   |
+--------------------------------------------------------------------------+
```

### 4.2 Data-Segment Layout (Shared Library)

```
+-- DLL/SO ------------------------------------+   +-- EXE ------------------+
| at_exit.cpp defines:                          |   | Only calls public API:  |
|   g_top_manager_ = nullptr  (.bss)            |   |   RegisterCallback()   |
|   lock_                    (.data)            |   |   ProcessCallbacksNow()|
| stack_ (per AtExitManager instance)           |   | Never touches private  |
|                                               |   | static members directly|
| Guarantee: DLL data segment is process-unique |   | Links via import table |
+-----------------------------------------------+   +------------------------+
```

In shared-library builds, `g_top_manager_` and `lock_` reside in the library's
data segment, shared by all consumers across the process.

---

## 5. Static Library Multi-Module Linking (Deep Dive)

> **This is the most critical deployment constraint. Understand it before
> linking neixx as a static library into multiple binaries.**

### 5.1 Root Cause: Static Library Link Semantics

A static library (`.lib` / `.a`) is an archive of object files, not an OS load
unit. The linker **copies** required `.obj`/`.o` files into each final binary:

```
Before linking:
  neixx.lib
    +-- at_exit.obj
          |-- g_top_manager_  (definition)
          |-- lock_           (definition)
          +-- RegisterCallback() { ... }

After linking (EXE + DLL each link neixx.lib):

  myapp.exe                           myplugin.dll
  +--------------------------------+  +--------------------------------+
  | at_exit.obj COPY #1            |  | at_exit.obj COPY #2            |
  |   g_top_manager_ = 0x1000      |  |   g_top_manager_ = 0x2000      |
  |   lock_           instance #1  |  |   lock_           instance #2  |
  |   RegisterCallback() #1        |  |   RegisterCallback() #2        |
  +--------------------------------+  +--------------------------------+
       ^ independent data segment         ^ independent data segment
```

**Key fact**: `g_top_manager_`, `lock_`, and `stack_` are NOT "cross-module
shared globals" — they are **independent copies** per linked binary.

### 5.2 Failure Scenario

```cpp
// ========== myapp.exe ==========
int main() {
    nei::AtExitManager at_exit;          // g_top_manager_#1 = &at_exit
    LoadLibrary("myplugin.dll");
    PluginFunc();                        // calls into DLL
    return 0;
}  // ~AtExitManager: drains copy #1's stack only

// ========== myplugin.dll (also linked neixx.lib) ==========
void PluginFunc() {
    // RegisterCallback accesses copy #2!
    nei::AtExitManager::RegisterCallback([] {
        ReleasePluginResources();
    });
    // copy #2's g_top_manager_ is forever nullptr
    // -> returns false, callback silently dropped
}
```

### 5.3 Affected Components

This affects **every** component with file-scope static/global variables:

| Component | Affected data | Failure mode |
|-----------|--------------|-------------|
| `AtExitManager` | `g_top_manager_`, `lock_` | DLL callbacks silently dropped |
| `Singleton<T,Traits>` | `instance_`, `lock_` per instantiation | DLL creates its own isolated instance |
| Any Meyers' Singleton | Function-local `static T instance` | Each binary gets its own instance |

### 5.4 Solution Matrix

| Solution | Use case | Cost |
|----------|---------|------|
| **A) Convention: link into EXE only** | Monolithic process, no plugin architecture | Zero runtime cost |
| **B) Shared-library build** (`BUILD_SHARED_LIBS=ON`) | Plugin/DLL architecture | Must ship DLL/SO |
| **C) OS-level shared memory** | DLLs must statically link neixx | `CreateFileMapping` + cross-module sync, extreme complexity |
| **D) PIMPL + exported factory** | ABI stability + cross-module | All access through vtable |

**Current recommendation**: Option A for development (matches Chromium's own
practice). Evaluate Option B before any release with plugin support.

---

## 6. Thread Safety & Deadlock Prevention

### 6.1 Lock Protocol

```
RegisterCallback(cb):          ProcessCallbacksNow():
  lock_.lock()                   lock_.lock()
    stack_.push_back(cb)           local.swap(stack_)   <- entire stack moved out
  lock_.unlock()                 lock_.unlock()          <- lock released HERE
                                 for (auto& cb : reverse(local))
                                   cb()                  <- executes outside lock
```

### 6.2 Why Out-of-Lock Execution Is Critical

If callbacks were invoked while holding the lock:

```cpp
// DANGEROUS anti-pattern
void ProcessCallbacksNow() {
    std::lock_guard<std::mutex> lock(lock_);
    while (!stack_.empty()) {
        auto cb = std::move(stack_.back());
        stack_.pop_back();
        cb();  // holding lock! if cb calls RegisterCallback -> DEADLOCK
    }
}
```

A callback may internally:
1. Call `RegisterCallback` -> attempt to acquire `lock_` -> **deadlock**
2. Trigger another thread's `ProcessCallbacksNow` -> **deadlock**
3. Destroy an object whose destructor calls `RegisterCallback` -> **deadlock**

The **swap-then-execute** pattern is Chromium's standard deadlock-avoidance
idiom, used in `AtExitManager`, `MessageLoop`, `TaskRunner`, and elsewhere.

### 6.3 ~AtExitManager Race Window

```
~AtExitManager() {
    ProcessCallbacksNow();     // drains all registered callbacks
    // small race window: another thread may register a new callback here
    lock_.lock();
    if (g_top_manager_ == this)
        g_top_manager_ = nullptr;  // after this, RegisterCallback returns false
    lock_.unlock();
}
```

Callbacks registered in this window are **never executed**. This is an
acceptable trade-off during process shutdown, also present in Chromium's
single-AtExitManager scenario.

---

## 7. Leaky Singleton & Race-on-Shutdown Defense

### 7.1 The Fatal Race Scenario

When `~AtExitManager` executes a callback that `delete`s a singleton, and a
background thread simultaneously wakes up and calls `GetInstance()`:

```
Main Thread                            Background I/O Thread
----------                             --------------------
~AtExitManager()
  ProcessCallbacksNow()
    executes "delete pool" callback
      ~IOBufferPool()
      g_pool = nullptr                 wakes up, needs a buffer
                                       IOBufferPool::GetInstance()
                                         g_pool == nullptr -> true
                                           new IOBufferPool()  <- re-created!
                                           RegisterCallback(delete)
                                             g_top_manager_ still non-null
                                             stack_.push_back(cb) SUCCESS
    callback complete
  lock_ -> g_top_manager_ = nullptr
  new pool's delete callback never executed -> LEAK
```

**Result**: a new `IOBufferPool` instance with its cached 4K/64K buffers is
permanently leaked — the cleanup callback was registered but `g_top_manager_`
was subsequently nullified.

### 7.2 Chromium's Solution: True Leaky Singleton

> **Never `delete` the singleton itself in an AtExitManager callback.**

A correct Leaky Singleton does exactly two things:
1. **Releases internal physical resources** (e.g., all `unique_ptr<char[]>` in `free_blocks`)
2. **Keeps the singleton shell pointer valid and accessible forever**

| Benefit | Description |
|---------|-------------|
| **Crash prevention** | Background threads find a valid pointer, never a dangling nullptr |
| **No cross-thread blocking** | No complex synchronization needed to quiesce all threads |
| **OS cleanup** | The singleton shell (~200 bytes) is reclaimed by the OS at process exit |

### 7.3 Shutdown State Transition

```
Before ~AtExitManager:
  IOBufferPool {
    buckets_ = [
      { block_size=4K,  free_blocks=[buf0, buf1, ..., buf255] },
      { block_size=64K, free_blocks=[buf0, buf1, ..., buf63]  },
    ]
  }

After PurgeMemory():
  IOBufferPool {
    buckets_ = [
      { block_size=4K,  free_blocks=[] },   <- all freed, physical memory to OS
      { block_size=64K, free_blocks=[] },   <- all freed, physical memory to OS
    ]
    // object itself (~200 bytes) still alive
  }

Process exit:
  OS reclaims IOBufferPool shell + all virtual address space
```

---

## 8. Singleton<T, Traits> Architecture

### 8.1 Design Goals

The `Singleton` template uses the **Traits policy pattern** to decouple
allocation, destruction, and thread safety. Switching traits changes the
singleton's lifecycle without modifying business code.

### 8.2 Traits Policies

```cpp
// Traditional singleton: deleted at exit
template <typename T>
struct DefaultSingletonTraits {
    static T* New()    { return new T(); }
    static void Delete(T* x) { delete x; }
};

// Leaky singleton: shell never deleted (prevents shutdown crash)
template <typename T>
struct LeakySingletonTraits {
    static T* New()    { return new T(); }
    static void Delete(T* /*x*/) { /* intentionally empty */ }
};
```

For types that need internal resource cleanup without shell deletion, provide a
**template specialization** in the `.cpp` file:

```cpp
// io_buffer.cpp
template <>
void LeakySingletonTraits<IOBufferPool>::Delete(IOBufferPool* x) {
    if (x) {
        x->PurgeMemory();  // free 4K/64K cached blocks
        // intentionally do NOT delete x
    }
}
```

### 8.3 Thread Safety: DCL with Acquire-Release Barriers

```
GetInstance():
  instance = instance_.load(memory_order_acquire)   <- fast path
  if (instance != nullptr) return instance;

  lock_.lock()                                      <- slow path
    instance = instance_.load(memory_order_relaxed)
    if (instance == nullptr) {
      instance = Traits::New()
      AtExitManager::RegisterCallback([] {
        Traits::Delete(instance_.load(relaxed))
      })
      CHECK_MSG(registered, "AtExitManager missing")
      instance_.store(instance, memory_order_release) <- publish
    }
  lock_.unlock()
  return instance;
```

| Barrier | Purpose |
|---------|---------|
| `acquire` | See all memory writes from the constructing thread |
| `release` | Ensure T's constructor completes before publishing the pointer |
| mutex | Provides additional acquire-release semantics inside the slow path |

### 8.4 Usage Pattern

```cpp
// Declaration (in .h)
class IOBufferPool {
public:
    static IOBufferPool& GetInstance();
private:
    IOBufferPool() = default;
    friend struct LeakySingletonTraits<IOBufferPool>;
};

// Implementation (in .cpp)
IOBufferPool& IOBufferPool::GetInstance() {
    return *Singleton<IOBufferPool, LeakySingletonTraits<IOBufferPool>>::GetInstance();
}
```

---

## 9. IOBufferPool Integration (Full Example)

### 9.1 Header (`io_buffer.h`)

```cpp
#include <neixx/common/singleton.h>

class NEI_API IOBufferPool {
public:
    static IOBufferPool& GetInstance();
    scoped_refptr<IOBufferWithSize> AcquireBuffer(std::size_t size);
    void PurgeMemory();

private:
    IOBufferPool();
    ~IOBufferPool();

    // Grant LeakySingletonTraits access to the private constructor and
    // allow the specialized Delete() to call PurgeMemory().
    friend struct LeakySingletonTraits<IOBufferPool>;

    struct Bucket { /* ... */ };
    mutable std::mutex lock_;
    std::vector<Bucket> buckets_;
};
```

### 9.2 Implementation (`io_buffer.cpp`)

```cpp
// Specialization: drain cached buffers, keep shell alive
template <>
void LeakySingletonTraits<IOBufferPool>::Delete(IOBufferPool* x) {
    if (x) {
        x->PurgeMemory();
        // intentionally do NOT delete x
    }
}

IOBufferPool& IOBufferPool::GetInstance() {
    return *Singleton<IOBufferPool, LeakySingletonTraits<IOBufferPool>>::GetInstance();
}
```

---

## 10. Best Practices & Anti-Patterns

### 10.1 Recommended Pattern

```cpp
int main() {
    nei::AtExitManager at_exit;          // 1. First line

    auto& pool = nei::IOBufferPool::GetInstance();  // 2. Init Leaky Singletons

    AtExitManager::RegisterCallback([] {  // 3. Register cleanup callbacks
        FlushPendingLogs();
    });

    RunApplication();                     // 4. Business logic

    return 0;                             // 5. ~AtExitManager auto-cleans
}
```

### 10.2 Anti-Patterns

| Anti-pattern | Consequence | Correct approach |
|---|---|---|
| AtExitManager not the first stack object | Callbacks may access already-destroyed objects | `at_exit` must be main()'s first statement |
| Creating AtExitManager in a DLL | EXE and DLL get independent managers | Only create in EXE's main() |
| Heavy I/O in callbacks | Blocks other callbacks, prolongs shutdown | Callbacks should do lightweight cleanup only |
| Relying on callback order for business logic | LIFO is convention, not API guarantee | Registration order = cleanup order only |
| **`delete`-ing a singleton in an AtExit callback** | Background thread re-entry -> UAF crash or orphan leak | Release internal resources only, keep shell alive |
| Not checking RegisterCallback return value | Callbacks silently dropped | `DCHECK(ok)` in debug; `CHECK_MSG` on critical paths |

### 10.3 Defensive Registration Wrapper

```cpp
void RegisterCleanup(std::function<void()> cb) {
    bool ok = nei::AtExitManager::RegisterCallback(std::move(cb));
    DCHECK(ok) << "AtExitManager::RegisterCallback failed. Possible causes:\n"
               << "  1. Called before main() created the AtExitManager.\n"
               << "  2. Called from a DLL with a separate static copy of neixx.";
}
```

### 10.4 Test Infrastructure

Tests need a global `AtExitManager`. The project provides a custom GTest main:

```cpp
// tests/test_main.cpp
#include <neixx/common/at_exit.h>
#include <gtest/gtest.h>

int main(int argc, char** argv) {
    nei::AtExitManager at_exit;  // covers the entire test process lifetime
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

Individual tests should NOT create their own `AtExitManager` — the global one
in `test_main.cpp` suffices. A second instance would trigger the duplicate
`CHECK` and abort.

---

## 11. Comparison with Chromium

### 11.1 AtExitManager

| Dimension | Chromium `base::AtExitManager` | `nei::AtExitManager` |
|-----------|-------------------------------|----------------------|
| Callback type | `base::OnceClosure` (move-only) | `std::function<void()>` |
| Namespace | `base::` | `nei::` |
| Registration return | `void` (CHECK on failure) | `bool` (caller decides) |
| Nested managers | Supported (linked list `next_manager_`) | Not supported (single process) |
| Location | `base/at_exit.h` | `neixx/common/include/neixx/common/at_exit.h` |

### 11.2 Singleton

| Dimension | Chromium `base::Singleton` | `nei::Singleton` |
|-----------|---------------------------|------------------|
| Traits model | `DefaultSingletonTraits` / `LeakySingletonTraits` / `StaticSingletonTraits` | Default + Leaky (extensible) |
| Memory barriers | `subtle::AtomicWord` + custom Acquire/Release | `std::atomic<T*>` + standard `memory_order` |
| AtExit integration | `base::AtExitManager::RegisterCallback` | `nei::AtExitManager::RegisterCallback` |
| Location | `base/memory/singleton.h` | `neixx/common/include/neixx/common/singleton.h` |

---

## 12. File Manifest

| File | Role |
|------|------|
| `modules/neixx/common/include/neixx/common/at_exit.h` | AtExitManager public API |
| `modules/neixx/common/include/neixx/common/singleton.h` | Singleton container + traits |
| `modules/neixx/common/src/at_exit.cpp` | AtExitManager implementation |
| `modules/neixx/io/include/neixx/io/io_buffer.h` | IOBufferPool declaration (friend traits) |
| `modules/neixx/io/src/io_buffer.cpp` | LeakySingletonTraits specialization + GetInstance |
| `demo/at_exit_demo.cpp` | Integration demo with background thread race test |
| `tests/test_main.cpp` | Custom GTest main with global AtExitManager |
| `docs/neixx_at_exit_technical.md` | This document |
