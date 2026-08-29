#pragma once

#ifndef NEIXX_SYNCHRONIZATION_ATOMIC_EVENT_H_
#define NEIXX_SYNCHRONIZATION_ATOMIC_EVENT_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>

namespace nei {

// =============================================================================
// AtomicEvent — single-word auto-reset event (futex / WaitOnAddress)
// =============================================================================
//
// A minimal auto-reset synchronization event whose entire state lives in
// one atomic 32-bit word: the Linux build parks waiters with the futex
// syscall, the Windows build uses WaitOnAddress.  Unlike WaitableEvent
// (heap state + mutex + condition variable), Signal/Wait perform NO lock
// handshake:
//
//   - Signal with no waiter:   one atomic CAS, no syscall.
//   - Wait with a parked token: one atomic CAS, no syscall.
//   - Contended Wait:          one futex / WaitOnAddress park.
//
// State word layout:
//   bit 0            : SIGNALED — a token is parked.
//   bit 1            : BROADCAST — irreversible broadcast (teardown); every
//                      Wait/TimedWait returns immediately without consuming
//                      it, so ALL waiters wake even with no token to share.
//   bits 2..31       : waiter count (threads currently parked or parking).
//
// Protocol (no lost wakeups):
//   - Wait:      sees SIGNALED -> CAS it away and return.  Otherwise
//                increments the waiter count, then parks the kernel wait
//                keyed on the count-adjusted word.  The kernel re-reads the
//                word before sleeping, so a Signal that lands between the
//                count increment and the park fails the comparison and the
//                wait returns immediately — the token is never missed.
//   - Signal:    already SIGNALED -> no-op.  Otherwise CAS the SIGNALED
//                bit in; if waiters were present, wake exactly one.  A
//                token parked with no waiter needs no syscall.
//   - Multiple waiters are safe: every Signal that observes a non-zero
//                waiter count wakes one waiter, matching auto-reset
//                semantics (one token, one consumer).
//
// Auto-reset semantics: a signal notifies exactly one waiter; a signal
// with no waiter is remembered for the next Wait (Windows-event style).
// A TimedWait that times out does NOT consume the token.  IsSignaled()
// reports whether a token is currently parked.
//
// Memory ordering: token handoff uses acquire/release, so everything the
// signaler wrote before Signal() is visible to the waiter that consumes
// the token.
//
// Windows version floor: WaitOnAddress is dynamically resolved via
// GetProcAddress (Win8+).  On older systems (e.g. Windows 7) the event
// transparently degrades to a heap CRITICAL_SECTION + CONDITION_VARIABLE
// fallback with identical semantics (only slower) — the library never
// hard-requires Win8 to LOAD.  POSIX always uses the single-word path.
//
// Non-copyable, non-movable (the word address is the kernel wait key).
class NEI_API AtomicEvent final {
public:
  AtomicEvent();
  ~AtomicEvent();

  AtomicEvent(const AtomicEvent &) = delete;
  AtomicEvent &operator=(const AtomicEvent &) = delete;
  AtomicEvent(AtomicEvent &&) = delete;
  AtomicEvent &operator=(AtomicEvent &&) = delete;

  // Wakes one waiter, or parks a token for the next Wait.
  void Signal();

  // Broadcast for teardown: sets an IRREVERSIBLE broadcast state and wakes
  // every currently parked waiter; every future Wait/TimedWait returns
  // immediately (without consuming anything).  Unlike Signal, this is NOT
  // idempotent-per-token — it turns the event into a permanently-open
  // gate, which is exactly what shutdown needs: each sleeper re-checks its
  // shutdown predicate and exits, no matter how many waiters there are.
  // Use it only when the event will never be used again (or wrap the
  // object's lifetime accordingly).
  void SignalAll();

  // Blocks until a token is available; consumes it.
  void Wait();

  // Blocks until a token is available or |timeout| elapses.  Returns true
  // if a token was consumed, false on timeout (the token, if any, is
  // left parked).  Spurious wakeups are retried internally.
  bool TimedWait(std::chrono::milliseconds timeout);

  // True if a token is currently parked (not yet consumed by a Wait).
  bool IsSignaled() const;

private:
  enum : uint32_t {
    kSignaledBit = 1u,
    kBroadcastBit = 2u,
    // Waiter count occupies the remaining bits (one waiter == 4).
    kWaiterIncrement = 4u,
    kParkMask = kSignaledBit | kBroadcastBit,
  };

  // Parks on |expected| until the word changes or |timeout| (null =
  // forever).  Returns true if woken (or the word already differs),
  // false on timeout.
  bool Park(uint32_t expected, const std::chrono::milliseconds *timeout);

  // Bounded spin of plain loads before registering as a waiter (see the
  // implementation).  Returns true if a token / the broadcast gate appeared.
  bool SpinForToken() const;
  static constexpr int kSpinBeforeParkLoads = 16;

  // Kernel wake used by Signal / SignalAll (one vs all waiters).
  void WakeSome(uint32_t count, std::atomic<uint32_t> *word);

  // Windows-only: heap fallback for systems without WaitOnAddress
  // (non-null only when the single-word kernel path is unavailable).
  struct Impl;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> fallback_;
  std::atomic<uint32_t> state_{0};
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

} // namespace nei

#endif // NEIXX_SYNCHRONIZATION_ATOMIC_EVENT_H_
