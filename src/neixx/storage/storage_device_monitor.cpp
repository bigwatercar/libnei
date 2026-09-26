#include <neixx/storage/storage_device_monitor.h>

#if defined(_WIN32)
#include "storage_device_monitor_win.h"
#else
#include "storage_device_monitor_posix.h"
#endif

namespace nei {

StorageDeviceMonitor::StorageDeviceMonitor(
    scoped_refptr<SingleThreadTaskRunner> task_runner, Callback callback)
    : impl_(std::make_unique<Impl>(std::move(task_runner), std::move(callback))) {
}

StorageDeviceMonitor::~StorageDeviceMonitor() = default;

bool StorageDeviceMonitor::Start() {
  return impl_->Start();
}

void StorageDeviceMonitor::Stop() {
  impl_->Stop();
}

void StorageDeviceMonitor::EmitEventForTesting(DeviceEventType type,
                                                const DeviceInfo& info) {
  impl_->EmitEvent(type, info);
}

} // namespace nei
