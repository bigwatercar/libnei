#pragma once

#ifndef NEIXX_STORAGE_STORAGE_DEVICE_MONITOR_POSIX_H_
#define NEIXX_STORAGE_STORAGE_DEVICE_MONITOR_POSIX_H_

#if !defined(_WIN32)

#include <string>

#include <neixx/common/location.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/storage/storage_device_monitor.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/task_runner.h>

namespace nei {

// One NETLINK_KOCKET_UEVENT netlink message parsed into plain fields.  The
// attribute types are defined by the kernel (kernel/netlink/uevent.c);
// libudev is not involved.
struct UeventMsg {
  std::string action;
  std::string subsystem;
  std::string devtype;
  std::string devname;
  std::string devnode;
  std::string devpath;
};

class StorageDeviceMonitor::Impl final : public MessagePumpForIO::Watcher {
 public:
  explicit Impl(scoped_refptr<TaskRunner> task_runner,
                Callback callback);
  ~Impl() override;

  bool Start();
  void Stop();

  bool is_watching() const {
    return watching_;
  }

  // MessagePumpForIO::Watcher:
  void OnFileCanReadWithoutBlocking(NativeIOHandle handle) override;
  void OnFileCanWriteWithoutBlocking(NativeIOHandle) override {
  }

  // Internal hook: emit an event through the same dispatch path used by the
  // raw netlink source.  Also used by tests to inject synthetic events.
  void EmitEvent(DeviceEventType type, const DeviceInfo& info);

  // Drain and dispatch all currently-received kernel uevents.
  void DrainUdevEvents();

  void HandleUdevMessage(const UeventMsg& message);

  void CleanupUdev();

 private:
  static int CreateNetlinkSocket();

  scoped_refptr<TaskRunner> task_runner_;

  int udev_fd_ = -1;
  MessagePumpForIO::FdWatchController controller_;

  Callback callback_;
  bool watching_ = false;

  WeakPtrFactory<Impl> weak_factory_;
};

}  // namespace nei

template <>
struct nei::WeakPtrThreadSafe<nei::StorageDeviceMonitor::Impl> : std::true_type {};

#endif // !defined(_WIN32)
#endif // NEIXX_STORAGE_STORAGE_DEVICE_MONITOR_POSIX_H_
