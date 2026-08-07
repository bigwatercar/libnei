#pragma once
#ifndef NEIXX_MEMORY_MEMORY_PRESSURE_MONITOR_H_
#define NEIXX_MEMORY_MEMORY_PRESSURE_MONITOR_H_

#include <nei/build/nei_export.h>
#include <nei/build/compiler_specific.h>

#include <memory>

namespace nei {

// =============================================================================
// MemoryPressureMonitor — a Chromium-style memory-pressure component.
//
// This is a generic, public base component decoupled from any allocator:
//   * GetCurrentMemoryPressureLevel()  — queries the current system memory
//     pressure (NONE / MODERATE / CRITICAL).
//   * MemoryPressureListenerRegistry   — a thread-safe registry of observers
//     that are fanned out on Notify(level).
//   * MemoryPressureMonitor            — a stateful wrapper: PollOnce()
//     re-samples the level and automatically notifies listeners on change.
//
// The application (the "main program") is responsible for assembling the
// pipeline: sample the level from its own low-frequency hook (timer, event
// loop or OS signal), call Notify() when the level changes, and have a
// listener drive reclamation (e.g. PurgeSmallObjectAllocator()).  This mirrors
// Chromium, where base::MemoryPressureMonitor detects pressure and listeners
// (including PartitionAlloc) react with PurgeMemory.
// =============================================================================

/// Chromium-style memory pressure levels.
enum class MemoryPressureLevel {
  kNone,     // no pressure
  kModerate, // system memory getting low — reclaim cheap resources
  kCritical, // system memory severely constrained — drop all reclaimable memory
};

/// Queries the current system memory-pressure level.  No allocation and no
/// side effects.  Platform strategy (mirrors Chromium):
///   * Windows — poll the dynamic commit limit via GlobalMemoryStatusEx
///     (committed memory vs. commit limit) plus physical-memory usage.
///   * macOS — zero-threshold: delegate entirely to the XNU kernel via
///     sysctl kern.memorystatus_vm_pressure_level.
///   * Linux — MemAvailable as the primary signal, softened by the reclaimable
///     page-cache ratio (Cached + SReclaimable).
///   * Other POSIX — available / total physical memory ratio.
/// Returns kNone when the information is unavailable.
NEI_API MemoryPressureLevel GetCurrentMemoryPressureLevel();

/// Abstract observer of memory-pressure changes.  Applications wire a listener
/// to reclamation logic; the monitor itself is decoupled from any allocator.
class MemoryPressureListener {
public:
  virtual ~MemoryPressureListener() = default;
  virtual void OnMemoryPressure(MemoryPressureLevel level) = 0;
};

/// Thread-safe registry of MemoryPressureListeners (Chromium's
/// MemoryPressureListenerRegistry).  The application's own pressure hook calls
/// Notify() whenever GetCurrentMemoryPressureLevel() changes, which fans the
/// level out to every registered listener.
class MemoryPressureListenerRegistry {
public:
  MemoryPressureListenerRegistry() = delete;
  MemoryPressureListenerRegistry(const MemoryPressureListenerRegistry &) = delete;
  MemoryPressureListenerRegistry &operator=(const MemoryPressureListenerRegistry &) = delete;

  /// Registers a listener.  No-op for null.  Duplicate registrations are not
  /// deduplicated (caller should Add once and Remove on destruction).
  static NEI_API void Add(MemoryPressureListener *listener);

  /// Removes a previously registered listener.  Safe to call with a listener
  /// that was never added.
  static NEI_API void Remove(MemoryPressureListener *listener);

  /// Fans `level` out to a snapshot of all registered listeners (listeners are
  /// invoked outside the registry lock).
  static NEI_API void Notify(MemoryPressureLevel level);
};

/// Stateful Chromium-style memory pressure monitor.  The application drives it
/// from its own low-frequency hook (timer, event loop or OS signal) by calling
/// PollOnce(): it re-samples the system level and, when the level changes,
/// fans it out to every registered MemoryPressureListener.  No internal timer
/// or background thread — sampling is caller-driven.
class NEI_API MemoryPressureMonitor {
public:
  MemoryPressureMonitor();
  ~MemoryPressureMonitor();
  MemoryPressureMonitor(const MemoryPressureMonitor &) = delete;
  MemoryPressureMonitor &operator=(const MemoryPressureMonitor &) = delete;

  /// Re-samples the system level via GetCurrentMemoryPressureLevel().  If it
  /// differs from the last known level, notifies the
  /// MemoryPressureListenerRegistry.  Returns the current level.
  MemoryPressureLevel PollOnce();

  /// Returns the most recently sampled level (does not re-sample).
  MemoryPressureLevel GetCurrentPressureLevel() const;

private:
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

} // namespace nei

#endif // NEIXX_MEMORY_MEMORY_PRESSURE_MONITOR_H_
