#if defined(_WIN32)

#include "storage_monitor_impl.h"

namespace nei {

// Windows backend is not implemented yet.
bool StorageMonitor::Impl::PlatformStart() {
  return false;
}

void StorageMonitor::Impl::PlatformStop() {
}

std::vector<StorageInfo> StorageMonitor::Impl::PlatformScan() const {
  return {};
}

} // namespace nei

#endif // _WIN32
