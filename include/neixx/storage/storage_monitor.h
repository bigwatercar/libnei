#pragma once

#ifndef NEIXX_STORAGE_STORAGE_MONITOR_H_
#define NEIXX_STORAGE_STORAGE_MONITOR_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nei/build/nei_export.h>
#include <neixx/functional/callback.h>
#include <neixx/task/task_runner.h>

namespace nei {

// Description of one mounted storage volume.  All strings are UTF-8.
struct StorageInfo {
  enum class Type {
    kRemovableMassStorage,
    kFixedMassStorage,
  };

  Type type = Type::kFixedMassStorage;
  // Stable across attach/detach cycles; treat as opaque.  Linux, in order of
  // preference: "uuid:<filesystem uuid>", "partuuid:<partition uuid>",
  // "wwid:<disk wwid>p<partition>" / "serial:<disk serial>p<partition>", or
  // "path:<device node>" when the device exposes no identifier at all (that
  // last form can change when the device is re-plugged).  Windows:
  // "volume:{GUID}" or "serial:<hex>".
  std::string device_id;
  // Current mount point (Windows: drive root such as "E:\").
  std::string location;
  std::string label;
  std::string vendor;
  std::string model;
  std::uint64_t total_size_bytes = 0;
};

// Receives attach/detach notifications for REMOVABLE volumes only.
// Always invoked on the sequence of the monitor's task runner.
class NEI_API RemovableStorageObserver {
public:
  virtual ~RemovableStorageObserver() = default;

  virtual void OnRemovableStorageAttached(const StorageInfo &info) = 0;
  virtual void OnRemovableStorageDetached(const StorageInfo &info) = 0;
};

// StorageMonitor tracks mounted storage volumes and reports removable ones
// coming and going.  Modeled after Chromium's components/storage_monitor.
//
// Platform backends:
//   Linux    - /proc/self/mountinfo change notification via epoll
//              (requires a MessagePumpForIO); volume metadata from sysfs and
//              /dev/disk.  No libudev dependency.
//   Windows  - drive letters (A:..Z:) re-enumerated on volume interface
//              events.  Win8+ uses CM_Register_Notification (no window);
//              Windows 7 falls back to a hidden top-level window receiving
//              WM_DEVICECHANGE on a dedicated thread.  Does not need a
//              MessagePumpForIO.  Volume info is queried synchronously.
//
// Thread-safety: every method, including the destructor, must be called on
// the sequence of the TaskRunner passed to the constructor, and that runner
// must be backed by a MessagePumpForIO on Linux.  Observers are invoked on the same
// sequence, so no locking is involved; Add/RemoveObserver may be called from
// inside an observer callback.
class NEI_API StorageMonitor final {
public:
  // Overridable inputs, intended for tests.
  struct Options {
    std::string mountinfo_path = "/proc/self/mountinfo";
    std::string sysfs_root = "/sys";
    std::string dev_disk_root = "/dev/disk";
    // false: do not register for change events; drive updates with Rescan().
    bool watch_changes = true;
    // Windows only: skip CM_Register_Notification and use the Win7 window path.
    bool use_legacy_window_notifications = false;
    // Run after each event-driven rescan (one event may trigger two: an
    // immediate one and a delayed one on Windows).  Not run by Rescan().
    RepeatingClosure on_event_rescan_for_testing;
  };

  explicit StorageMonitor(scoped_refptr<SingleThreadTaskRunner> task_runner);
  StorageMonitor(scoped_refptr<SingleThreadTaskRunner> task_runner, Options options);
  ~StorageMonitor();

  StorageMonitor(const StorageMonitor &) = delete;
  StorageMonitor &operator=(const StorageMonitor &) = delete;
  StorageMonitor(StorageMonitor &&) = delete;
  StorageMonitor &operator=(StorageMonitor &&) = delete;

  // Performs the initial enumeration synchronously (no observer callbacks for
  // volumes that already exist) and begins watching for changes.  Returns
  // false if the platform is unsupported or the watch could not be set up.
  bool Start();

  // Stops watching.  Known volumes are kept until the next Start().
  void Stop();

  bool is_started() const;

  void AddObserver(RemovableStorageObserver *observer);
  void RemoveObserver(RemovableStorageObserver *observer);

  // All known volumes, fixed and removable.
  std::vector<StorageInfo> GetAllAvailableStorages() const;

  // Finds the volume containing |path| (longest mount-point match).  Returns
  // false if |path| is not on a known volume.
  bool GetStorageInfoForPath(const std::string &path, StorageInfo *info) const;

  // Re-reads the mount table and notifies observers of differences.  Called
  // automatically on change events; exposed for Options::watch_changes=false.
  void Rescan();

private:
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

} // namespace nei

#endif // NEIXX_STORAGE_STORAGE_MONITOR_H_
