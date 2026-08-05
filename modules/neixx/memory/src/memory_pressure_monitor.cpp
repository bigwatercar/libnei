// memory_pressure_monitor.cpp — implementation of the Chromium-style memory
// pressure component.  Detection and notification details are private to this
// TU; the public API is declared in memory_pressure_monitor.h.

#include <neixx/memory/memory_pressure_monitor.h>

#include <nei/sys/memory_info.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <cstdio>
#endif

namespace nei {
namespace {

struct Registry {
  std::mutex m;
  std::vector<MemoryPressureListener *> listeners;
};

Registry &GetRegistry() {
  static Registry r;
  return r;
}

} // namespace

MemoryPressureLevel GetCurrentMemoryPressureLevel() {
#if defined(_WIN32)
  // Windows (Chromium-style): poll the dynamic commit limit plus physical-
  // memory usage.  GlobalMemoryStatusEx reports ullTotalPageFile (approximate
  // commit limit) and ullAvailPageFile (committable without extending the
  // page file), so committed = total - available.
  MEMORYSTATUSEX status;
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) {
    return MemoryPressureLevel::kNone;
  }
  const std::uint64_t commit_limit = status.ullTotalPageFile;
  if (commit_limit == 0 || status.ullTotalPhys == 0) {
    return MemoryPressureLevel::kNone;
  }
  const std::uint64_t commit_used =
      status.ullTotalPageFile - status.ullAvailPageFile;
  const std::uint64_t commit_pct = commit_used * 100 / commit_limit;
  const std::uint64_t phys_pct =
      (status.ullTotalPhys - status.ullAvailPhys) * 100 / status.ullTotalPhys;
  if (commit_pct >= 90 || phys_pct >= 95) {
    return MemoryPressureLevel::kCritical;
  }
  if (commit_pct >= 75 || phys_pct >= 90) {
    return MemoryPressureLevel::kModerate;
  }
  return MemoryPressureLevel::kNone;

#elif defined(__APPLE__)
  // macOS (Chromium-style): zero-threshold — delegate entirely to the XNU
  // kernel.  kern.memorystatus_vm_pressure_level reports 0=normal, 1=warn,
  // 2=critical.
  int kernel_level = 0;
  std::size_t size = sizeof(kernel_level);
  if (sysctlbyname("kern.memorystatus_vm_pressure_level", &kernel_level, &size,
                   nullptr, 0) != 0) {
    return MemoryPressureLevel::kNone;
  }
  if (kernel_level >= 2) {
    return MemoryPressureLevel::kCritical;
  }
  if (kernel_level >= 1) {
    return MemoryPressureLevel::kModerate;
  }
  return MemoryPressureLevel::kNone;

#elif defined(__linux__)
  // Linux (Chromium-style): MemAvailable as the primary signal, softened by
  // the reclaimable page-cache ratio (Cached + SReclaimable).
  const std::uint64_t total = nei_get_total_physical_memory();
  const std::uint64_t available = nei_get_available_physical_memory();
  if (total == 0) {
    return MemoryPressureLevel::kNone;
  }
  std::uint64_t cached_kb = 0;
  FILE *f = std::fopen("/proc/meminfo", "r");
  if (f != nullptr) {
    char line[256];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
      unsigned long long v = 0;
      if (std::sscanf(line, "Cached: %llu kB", &v) == 1) {
        cached_kb += v;
      } else if (std::sscanf(line, "SReclaimable: %llu kB", &v) == 1) {
        cached_kb += v;
      }
    }
    std::fclose(f);
  }
  const double free_ratio =
      static_cast<double>(available) / static_cast<double>(total);
  const double cache_ratio =
      static_cast<double>(cached_kb * 1024) / static_cast<double>(total);
  // Reclaimable page cache is only partially recoverable (dirty pages must be
  // written back first), so count half of it toward effective available memory.
  const double effective_free = free_ratio + cache_ratio * 0.5;
  if (effective_free < 0.05) {
    return MemoryPressureLevel::kCritical;
  }
  if (effective_free < 0.10) {
    return MemoryPressureLevel::kModerate;
  }
  return MemoryPressureLevel::kNone;

#else
  // Fallback (other POSIX): available / total physical memory ratio.
  const std::uint64_t total = nei_get_total_physical_memory();
  if (total == 0) {
    return MemoryPressureLevel::kNone;
  }
  const std::uint64_t available = nei_get_available_physical_memory();
  const std::uint64_t free_pct = available * 100 / total;
  if (free_pct <= 5) {
    return MemoryPressureLevel::kCritical;
  }
  if (free_pct <= 10) {
    return MemoryPressureLevel::kModerate;
  }
  return MemoryPressureLevel::kNone;
#endif
}

void MemoryPressureListenerRegistry::Add(MemoryPressureListener *listener) {
  if (listener == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(GetRegistry().m);
  GetRegistry().listeners.push_back(listener);
}

void MemoryPressureListenerRegistry::Remove(MemoryPressureListener *listener) {
  if (listener == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(GetRegistry().m);
  std::vector<MemoryPressureListener *> &v = GetRegistry().listeners;
  for (auto it = v.begin(); it != v.end(); ++it) {
    if (*it == listener) {
      v.erase(it);
      break;
    }
  }
}

void MemoryPressureListenerRegistry::Notify(MemoryPressureLevel level) {
  std::vector<MemoryPressureListener *> snapshot;
  {
    std::lock_guard<std::mutex> lock(GetRegistry().m);
    snapshot = GetRegistry().listeners;
  }
  for (MemoryPressureListener *listener : snapshot) {
    if (listener != nullptr) {
      listener->OnMemoryPressure(level);
    }
  }
}

// ---- MemoryPressureMonitor (stateful wrapper) -------------------------------

class MemoryPressureMonitor::Impl {
public:
  std::atomic<MemoryPressureLevel> current_level{MemoryPressureLevel::kNone};
};

MemoryPressureMonitor::MemoryPressureMonitor() : impl_(new Impl) {}
MemoryPressureMonitor::~MemoryPressureMonitor() = default;

MemoryPressureLevel MemoryPressureMonitor::PollOnce() {
  const MemoryPressureLevel level = GetCurrentMemoryPressureLevel();
  if (level != impl_->current_level.load(std::memory_order_relaxed)) {
    impl_->current_level.store(level, std::memory_order_relaxed);
    MemoryPressureListenerRegistry::Notify(level);
  }
  return level;
}

MemoryPressureLevel MemoryPressureMonitor::GetCurrentPressureLevel() const {
  return impl_->current_level.load(std::memory_order_relaxed);
}

} // namespace nei
