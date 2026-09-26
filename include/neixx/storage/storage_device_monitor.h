#pragma once

#ifndef NEIXX_STORAGE_STORAGE_DEVICE_MONITOR_H_
#define NEIXX_STORAGE_STORAGE_DEVICE_MONITOR_H_

#include <memory>
#include <string>

#include <nei/build/nei_export.h>
#include <neixx/functional/callback.h>
#include <neixx/task/task_runner.h>

namespace nei {

// ---------------------------------------------------------------------------
// StorageDeviceMonitor — cross-platform storage-device event monitoring.
//
// Notifies when a whole-disk (e.g. /dev/sda, /dev/nvme0n1) is added,
// removed, or changed.  The callback is always invoked on the bound
// TaskRunner.
//
// Platform backends:
//   Linux — libudev netlink (monitors "block" subsystem, DEVTYPE "disk")
//   Windows — (reserved; not implemented in this revision)
//
// Usage:
//   auto monitor = std::make_unique<StorageDeviceMonitor>(io_runner,
//       [](StorageDeviceMonitor::DeviceEventType type,
//          const StorageDeviceMonitor::DeviceInfo& info) {
//     // info.device_path e.g. "/dev/sdb", info.name e.g. "sdb"
//   });
//   // Must run on the sequence of io_runner:
//   io_runner->PostTask(FROM_HERE, [&] { monitor->Start(); });
//   // ... later:
//   io_runner->PostTask(FROM_HERE, [&] { monitor->Stop(); });
//
// Thread-safety:
//   - Start() / Stop() must be called on the same sequence as the TaskRunner
//     passed to the constructor.
//   - The callback is always invoked on that TaskRunner.
// ---------------------------------------------------------------------------
class NEI_API StorageDeviceMonitor final {
 public:
  enum class DeviceEventType {
    kAdded,    // A whole-disk device appeared.
    kRemoved,  // A whole-disk device was removed.
    kChanged,  // A whole-disk device changed.
  };

  // Information about the affected device.
  struct DeviceInfo {
    std::string device_path;  // e.g. /dev/sdb
    std::string name;         // e.g. sdb
    std::string subsystem;    // e.g. block
  };

  // Callback signature:
  //   void(DeviceEventType type, const DeviceInfo& info)
  // May be invoked multiple times (once per detected event). Always invoked
  // on the bound TaskRunner.
  using Callback = RepeatingCallback<void(DeviceEventType, const DeviceInfo&)>;

  // Constructs a monitor that dispatches callbacks on |task_runner|.
  // |task_runner| must be backed by a MessagePumpForIO on platforms that
  // require an I/O pump.  The |callback| persists for the lifetime of the
  // watch and is NOT moved-from after invocation.
  StorageDeviceMonitor(scoped_refptr<SingleThreadTaskRunner> task_runner,
                       Callback callback);

  ~StorageDeviceMonitor();

  // Non-copyable, non-movable (internal Pimpl pointer stability required by
  // the pump watcher interfaces).
  StorageDeviceMonitor(const StorageDeviceMonitor&) = delete;
  StorageDeviceMonitor& operator=(const StorageDeviceMonitor&) = delete;
  StorageDeviceMonitor(StorageDeviceMonitor&&) = delete;
  StorageDeviceMonitor& operator=(StorageDeviceMonitor&&) = delete;

  // Begins monitoring storage-device events.
  //
  // Must be called on the sequence of the TaskRunner passed to the
  // constructor.  Returns true if monitoring was successfully started.  If
  // a previous monitor was active this is effectively a restart.
  bool Start();

  // Stops monitoring.  Safe to call multiple times; if no watch is active
  // this is a no-op.  After Stop() returns no further callbacks will fire.
  void Stop();

  // Test-only: emit an event through the normal dispatch path (used by unit
  // tests to verify callback delivery without real hardware).  Like the real
  // source, it is a no-op when not monitoring.
  void EmitEventForTesting(DeviceEventType type, const DeviceInfo& info);

 private:
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

} // namespace nei

#endif // NEIXX_STORAGE_STORAGE_DEVICE_MONITOR_H_
