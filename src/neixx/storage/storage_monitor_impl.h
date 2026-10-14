#pragma once

#ifndef NEIXX_STORAGE_STORAGE_MONITOR_IMPL_H_
#define NEIXX_STORAGE_STORAGE_MONITOR_IMPL_H_

#include <map>
#include <string>
#include <vector>

#include <neixx/storage/storage_monitor.h>
#include <neixx/task/message_loop/message_pump_io.h>

namespace nei {

class StorageMonitor::Impl final : public MessagePumpForIO::Watcher {
public:
  Impl(scoped_refptr<SingleThreadTaskRunner> task_runner, Options options);
  ~Impl() override;

  bool Start();
  void Stop();

  bool is_started() const {
    return started_;
  }

  void AddObserver(RemovableStorageObserver *observer);
  void RemoveObserver(RemovableStorageObserver *observer);

  std::vector<StorageInfo> GetAllAvailableStorages() const;
  bool GetStorageInfoForPath(const std::string &path, StorageInfo *info) const;
  void Rescan();

  // MessagePumpForIO::Watcher:
  void OnFileCanReadWithoutBlocking(NativeIOHandle) override {
  }

  void OnFileCanWriteWithoutBlocking(NativeIOHandle handle) override;

private:
  // Platform hooks (storage_monitor_posix.cpp / storage_monitor_win.cpp).
  bool PlatformStart();
  void PlatformStop();
  std::vector<StorageInfo> PlatformScan() const;

  // Replaces the known set; when |notify| is true, reports removable volumes
  // whose device_id or mount point changed.
  void ApplySnapshot(std::vector<StorageInfo> next, bool notify);

  scoped_refptr<SingleThreadTaskRunner> task_runner_;
  const Options options_;
  bool started_ = false;

  // device_id -> info; one entry per device even if mounted several times.
  std::map<std::string, StorageInfo> storages_;
  std::vector<RemovableStorageObserver *> observers_;

  MessagePumpForIO::FdWatchController controller_;
#if !defined(_WIN32)
  int mount_fd_ = -1;
#endif
};

} // namespace nei

#endif // NEIXX_STORAGE_STORAGE_MONITOR_IMPL_H_
