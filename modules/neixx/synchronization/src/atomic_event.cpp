#include <neixx/synchronization/atomic_event.h>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <ctime>
#endif

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace nei {

// Windows-only heap fallback (see atomic_event.h): CRITICAL_SECTION +
// CONDITION_VARIABLE (Vista+) with an explicit signaled flag implements the
// same auto-reset token semantics for systems without WaitOnAddress.  The
// condition-variable wait is atomic with releasing the lock, so the token
// check + park has no lost-wakeup window (the classic cv protocol).
//
// |signaled| is intentionally a plain bool, NOT std::atomic: every access
// (Signal / Wait / TimedWait / IsSignaled) happens under cs_ — the lock
// provides all mutual exclusion and happens-before.  Atomicity would only
// matter for the lock-free single-word path (state_ in the header), which
// is never used together with this fallback.
struct AtomicEvent::Impl {
#if defined(_WIN32)
  CRITICAL_SECTION cs;
  CONDITION_VARIABLE cv;
  bool signaled = false;
  bool broadcast = false; // SignalAll: irreversible open gate.
#endif
};

namespace {

#if defined(_WIN32)

using WaitOnAddressFn = BOOL(WINAPI *)(volatile void *, const void *, SIZE_T, DWORD);
using WakeByAddressSingleFn = VOID(WINAPI *)(PVOID);
using WakeByAddressAllFn = VOID(WINAPI *)(PVOID);

// Resolve WaitOnAddress / WakeByAddress* at run time (Win8+ only).
// Static references would make the whole library fail to LOAD on Windows 7
// (missing entry point), so the resolution follows the same pattern as
// PlatformThread::SetName (see docs/windows7_compatibility.md).
struct WaitOnAddressFns {
  WaitOnAddressFn wait = nullptr;
  WakeByAddressSingleFn wake = nullptr;
  WakeByAddressAllFn wake_all = nullptr;
};

const WaitOnAddressFns &GetWaitOnAddressFns() {
  static const WaitOnAddressFns fns = []() {
    WaitOnAddressFns out;
    // WaitOnAddress lives in the Synchronization apiset (Win8+): on some
    // Windows builds GetProcAddress(kernel32.dll, ...) misses the
    // forwarded exports, so try the canonical host modules in order.
    const wchar_t *kModules[] = {L"kernel32.dll", L"kernelbase.dll", L"api-ms-win-core-synch-l1-2-0.dll"};
    for (const wchar_t *name : kModules) {
      HMODULE mod = ::GetModuleHandleW(name);
      if (!mod)
        continue;
      if (!out.wait)
        out.wait = reinterpret_cast<WaitOnAddressFn>(::GetProcAddress(mod, "WaitOnAddress"));
      if (!out.wake)
        out.wake = reinterpret_cast<WakeByAddressSingleFn>(::GetProcAddress(mod, "WakeByAddressSingle"));
      if (!out.wake_all)
        out.wake_all = reinterpret_cast<WakeByAddressAllFn>(::GetProcAddress(mod, "WakeByAddressAll"));
      if (out.wait && out.wake && out.wake_all)
        break;
    }
    return out;
  }();
  return fns;
}

// WaitOnAddress compares the full word under the hood — the value check
// that closes the lost-wakeup window lives in the kernel.  Only reached
// when the single-word path is usable (fallback_ == null); the guards
// below are defense in depth.
bool ParkWord(std::atomic<uint32_t> *word, uint32_t expected, const std::chrono::milliseconds *timeout) {
  const WaitOnAddressFns &fns = GetWaitOnAddressFns();
  if (!fns.wait)
    return true; // Unreachable with the fallback armed; treat as a wake.
  DWORD ms = timeout ? static_cast<DWORD>((*timeout).count()) : INFINITE;
  BOOL ok = fns.wait(word, &expected, sizeof(uint32_t), ms);
  if (ok)
    return true;
  // Only ERROR_TIMEOUT means "timed out"; anything else is treated as a
  // wake (the caller re-checks the word).
  return GetLastError() != ERROR_TIMEOUT;
}

void WakeSomeWord(std::atomic<uint32_t> *word, uint32_t count) {
  const WaitOnAddressFns &fns = GetWaitOnAddressFns();
  if (!fns.wake)
    return;
  if (count <= 1 || !fns.wake_all) {
    fns.wake(word);
  } else {
    fns.wake_all(word);
  }
}

#else // Linux futex

bool ParkWord(std::atomic<uint32_t> *word, uint32_t expected, const std::chrono::milliseconds *timeout) {
  timespec ts{};
  timespec *tsp = nullptr;
  if (timeout) {
    ts.tv_sec = static_cast<time_t>((*timeout).count() / 1000);
    ts.tv_nsec = static_cast<long>(((*timeout).count() % 1000) * 1000000);
    tsp = &ts;
  }
  // FUTEX_WAIT re-checks *word == expected atomically inside the kernel:
  // a concurrent Signal changes the word, so the call returns immediately
  // instead of sleeping.  Retry semantics for EINTR are handled by the
  // caller's loop.
  long rv = syscall(SYS_futex, word, FUTEX_WAIT, expected, tsp, nullptr, 0);
  if (rv == 0)
    return true;
  return errno != ETIMEDOUT; // EINTR / EAGAIN -> treat as a wake, retry.
}

void WakeSomeWord(std::atomic<uint32_t> *word, uint32_t count) {
  // FUTEX_WAKE's third argument is an int; clamp defensively so a caller
  // passing UINT32_MAX ("wake everyone") never overflows to a negative
  // value — the kernel then wakes at most ONE waiter (ret >= nr_wake
  // trips immediately), silently degrading SignalAll to a single wake.
  syscall(SYS_futex, word, FUTEX_WAKE, static_cast<int>(std::min<uint32_t>(count, INT32_MAX)), nullptr, nullptr, 0);
}

#endif

} // namespace

AtomicEvent::AtomicEvent() {
#if defined(_WIN32)
  if (!GetWaitOnAddressFns().wait) {
    // Windows 7 (or older, without WaitOnAddress): degrade to the heap
    // CRITICAL_SECTION + CONDITION_VARIABLE fallback.  Identical auto-reset
    // semantics, slower only on these legacy systems.
    fallback_ = std::make_unique<Impl>();
    InitializeCriticalSection(&fallback_->cs);
    InitializeConditionVariable(&fallback_->cv);
  }
#endif
}

AtomicEvent::~AtomicEvent() {
#if defined(_WIN32)
  if (fallback_)
    DeleteCriticalSection(&fallback_->cs);
#endif
}

void AtomicEvent::Signal() {
#if defined(_WIN32)
  if (fallback_) {
    EnterCriticalSection(&fallback_->cs);
    if (fallback_->broadcast) {
      LeaveCriticalSection(&fallback_->cs);
      return; // Broadcasted: the gate is already open.
    }
    fallback_->signaled = true;
    WakeConditionVariable(&fallback_->cv);
    LeaveCriticalSection(&fallback_->cs);
    return;
  }
#endif
  uint32_t cur = state_.load(std::memory_order_acquire);
  for (;;) {
    if (cur & (kSignaledBit | kBroadcastBit))
      return; // A token is parked (or the gate is open) — nothing to do.
    uint32_t next = cur | kSignaledBit;
    if (state_.compare_exchange_weak(cur, next, std::memory_order_release, std::memory_order_acquire)) {
      // Parked waiters (count > 0) need an explicit kernel wake; a token
      // parked with no waiter is picked up by the waiter's own value
      // check, so no syscall is needed.  Wake exactly ONE waiter: the
      // auto-reset token has exactly one consumer, and waking more would
      // herd every parked waiter (all of which re-scan and go back to
      // sleep) — measurably catastrophic on futex-heavy systems (WSL2).
      if (cur >> 2)
        WakeSome(1, &state_);
      return;
    }
    // CAS failed: |cur| was reloaded with the latest value.
  }
}

void AtomicEvent::SignalAll() {
#if defined(_WIN32)
  if (fallback_) {
    EnterCriticalSection(&fallback_->cs);
    fallback_->broadcast = true;
    fallback_->signaled = true;
    WakeAllConditionVariable(&fallback_->cv);
    LeaveCriticalSection(&fallback_->cs);
    return;
  }
#endif
  for (;;) {
    uint32_t cur = state_.load(std::memory_order_acquire);
    uint32_t next = cur | kBroadcastBit;
    if (state_.compare_exchange_weak(cur, next, std::memory_order_release, std::memory_order_acquire)) {
      // Wake every parked waiter; latecomers see the broadcast bit and
      // return immediately.  The bit is never consumed.
      if (cur >> 2)
        WakeSome(static_cast<uint32_t>(std::numeric_limits<int>::max()), &state_);
      return;
    }
  }
}

void AtomicEvent::WakeSome(uint32_t count, std::atomic<uint32_t> *word) {
  WakeSomeWord(word, count);
}

bool AtomicEvent::SpinForToken() const {
  // Bounded spin of PLAIN LOADS before registering as a waiter: if a Signal
  // lands right after the failed token check above, the waiter finds the
  // parked token inside this window and consumes it WITHOUT a futex
  // round-trip (WSL2: ~112ns bare futex_wake + park latency per round).
  // Sized so an uncontended miss (a few ns per load) costs far less than
  // the syscall it avoids.  Deliberately NO pause instruction: on WSL2,
  // spinning with PAUSE triggers VM exits and measured -95% (see
  // docs/task_sync_review_20260815.md).
  for (int i = 0; i < kSpinBeforeParkLoads; ++i) {
    if (state_.load(std::memory_order_acquire) & (kSignaledBit | kBroadcastBit))
      return true;
  }
  return false;
}

bool AtomicEvent::Park(uint32_t expected, const std::chrono::milliseconds *timeout) {
  return ParkWord(&state_, expected, timeout);
}

void AtomicEvent::Wait() {
#if defined(_WIN32)
  if (fallback_) {
    // Classic cv protocol: checking the flag and parking are atomic with
    // releasing the lock, so a Signal between check and park is never lost.
    EnterCriticalSection(&fallback_->cs);
    while (!fallback_->signaled && !fallback_->broadcast)
      SleepConditionVariableCS(&fallback_->cv, &fallback_->cs, INFINITE);
    if (!fallback_->broadcast)
      fallback_->signaled = false;
    LeaveCriticalSection(&fallback_->cs);
    return;
  }
#endif
  for (;;) {
    uint32_t cur = state_.load(std::memory_order_acquire);
    if (cur & kBroadcastBit)
      return; // Broadcasted: the gate is open — return without consuming.
    if (cur & kSignaledBit) {
      uint32_t next = cur & ~kSignaledBit; // keep the broadcast bit + waiter count.
      if (state_.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire))
        return; // Token consumed.
      continue;
    }
    if (SpinForToken())
      continue; // A token landed in the spin window — consume it above.
    // Register as a waiter, then park keyed on the post-increment word
    // with the SIGNALED/BROADCAST bits masked OUT.  If a token arrives (or
    // was already parked) between the increment and the park, the kernel's
    // value check sees the mismatch and returns immediately instead of
    // sleeping — the token is never missed.  The count must include any
    // concurrent waiter (masking only the state bits preserves it).
    uint32_t expected = (state_.fetch_add(kWaiterIncrement, std::memory_order_relaxed) + kWaiterIncrement) & ~kParkMask;
    (void)Park(expected, nullptr);
    state_.fetch_sub(kWaiterIncrement, std::memory_order_relaxed);
    // Woken (spuriously or by Signal): retry — another waiter may have
    // consumed the token already.
  }
}

bool AtomicEvent::TimedWait(std::chrono::milliseconds timeout) {
#if defined(_WIN32)
  if (fallback_) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    EnterCriticalSection(&fallback_->cs);
    for (;;) {
      if (fallback_->broadcast) {
        LeaveCriticalSection(&fallback_->cs);
        return true; // Broadcasted: the gate is open.
      }
      if (fallback_->signaled) {
        fallback_->signaled = false;
        LeaveCriticalSection(&fallback_->cs);
        return true;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        LeaveCriticalSection(&fallback_->cs);
        return false; // Timed out without consuming the token.
      }
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      // Returns on wake (TRUE) or timeout (FALSE); loop re-checks the flag
      // and remaining budget either way (spurious wakes retried).
      SleepConditionVariableCS(&fallback_->cv, &fallback_->cs, static_cast<DWORD>(remaining.count()));
    }
  }
#endif
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    uint32_t cur = state_.load(std::memory_order_acquire);
    if (cur & kBroadcastBit)
      return true; // Broadcasted: the gate is open.
    if (cur & kSignaledBit) {
      uint32_t next = cur & ~kSignaledBit;
      if (state_.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire))
        return true;
      continue;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      return false; // Timed out without consuming the token.
    if (SpinForToken())
      continue; // A token landed in the spin window — consume it above.
    std::chrono::milliseconds remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    // Same masked-out-state-bits discipline as Wait(): a token granted
    // between the increment and the park fails the kernel value check
    // and returns immediately.
    uint32_t expected = (state_.fetch_add(kWaiterIncrement, std::memory_order_relaxed) + kWaiterIncrement) & ~kParkMask;
    bool woken = Park(expected, &remaining);
    state_.fetch_sub(kWaiterIncrement, std::memory_order_relaxed);
    if (!woken)
      return false;
    // Spurious or real wake: loop back to compete for the token.
  }
}

bool AtomicEvent::IsSignaled() const {
#if defined(_WIN32)
  if (fallback_) {
    EnterCriticalSection(&fallback_->cs);
    const bool s = fallback_->signaled || fallback_->broadcast;
    LeaveCriticalSection(&fallback_->cs);
    return s;
  }
#endif
  return (state_.load(std::memory_order_acquire) & (kSignaledBit | kBroadcastBit)) != 0;
}

} // namespace nei
