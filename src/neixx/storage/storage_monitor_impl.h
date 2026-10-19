#pragma once

#ifndef NEIXX_STORAGE_STORAGE_MONITOR_IMPL_H_
#define NEIXX_STORAGE_STORAGE_MONITOR_IMPL_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <neixx/storage/storage_monitor.h>
#include <neixx/task/message_loop/message_pump_io.h>

namespace nei {

// Everything a scan needs, copied so it can run on a worker without touching
// the Impl.
struct StorageScanConfig {
  std::string mountinfo_path;
  std::string sysfs_root;
  std::string dev_disk_root;
};

class StorageMonitor::Impl final : public MessagePumpForIO::Watcher {
public:
  using ScanConfig = StorageScanConfig;

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
  void EjectDevice(const std::string &device_id, OnceCallback<void(EjectStatus)> callback);

  // Entry point for change events; runs on the monitor sequence.
  void OnPlatformEvent();

  // MessagePumpForIO::Watcher:
  void OnFileCanReadWithoutBlocking(NativeIOHandle) override {
  }

  void OnFileCanWriteWithoutBlocking(NativeIOHandle handle) override;

private:
  // Lets worker-side continuations detect that the Impl is gone; |impl| is
  // only read and cleared on the monitor sequence.
  struct Alive {
    Impl *impl;
  };

  // Platform hooks (storage_monitor_posix.cpp / storage_monitor_win.cpp).
  // The static ones run on a worker and must not touch Impl state.
  bool PlatformStart();
  void PlatformStop();
  static std::vector<StorageInfo> PlatformScan(const ScanConfig &config);
  static EjectStatus PlatformEject(const std::string &location);

  // Resolves where |device_id| is mounted right now, re-reading the mount
  // table so a stale snapshot cannot redirect the eject to another volume.
  // Returns kOk and fills |location| when it is mounted and removable.
  static EjectStatus ResolveEjectTarget(const std::string &device_id, const ScanConfig &config, std::string *location);

  // Resolve + eject, touching no Impl state: used by the blocking sequence and
  // as the inline fallback when that sequence is unavailable.
  static EjectStatus RunEject(const std::string &device_id, const ScanConfig &config);

  // Caller-supplied blocking sequence, else one from the global thread pool.
  // Null when neither exists; blocking work then runs inline.
  scoped_refptr<SequencedTaskRunner> ResolveBlockingRunner() const;

  // Publishes a snapshot.  The first one after Start() describes volumes that
  // already existed, so it is applied silently and fires the
  // on_initial_enumeration_done hook; |notify| governs later snapshots.
  void PublishSnapshot(std::vector<StorageInfo> next, bool notify);
  void RunInitialEnumerationDoneHook();

  // Replaces the known set; when |notify| is true, reports removable volumes
  // whose device_id or mount point changed.
  void ApplySnapshot(std::vector<StorageInfo> next, bool notify);

  // Posts a scan to the blocking sequence; falls back to an inline scan when
  // that sequence rejects the task (e.g. the pool was shut down), so the
  // monitor cannot wedge with scan_in_flight_ stuck true.
  void StartAsyncScan();
  // |generation| identifies the scan that produced |result|; results from a
  // superseded scan are dropped so a late worker cannot revert a newer
  // snapshot (see Rescan()).
  void OnAsyncScanDone(std::uint64_t generation, std::vector<StorageInfo> result);
  void RunEventHook();
  void DeliverEjectResult(std::shared_ptr<OnceCallback<void(EjectStatus)>> callback, EjectStatus status);

  scoped_refptr<SingleThreadTaskRunner> task_runner_;
  const Options options_;
  const ScanConfig scan_config_;
  const std::shared_ptr<Alive> alive_;
  bool started_ = false;
  // True until the first snapshot after Start() has been published.
  bool initial_snapshot_pending_ = false;

  // Null when ThreadPoolInstance is not started; work then runs inline.
  scoped_refptr<SequencedTaskRunner> blocking_runner_;
  // Monotonic id of the newest scan.  Bumped by every Rescan() and by every
  // async scan start, so a result whose id is no longer current is stale.
  std::uint64_t scan_generation_ = 0;
  bool scan_in_flight_ = false;
  bool scan_dirty_ = false;

  // device_id -> info; one entry per device even if mounted several times.
  std::map<std::string, StorageInfo> storages_;
  std::vector<RemovableStorageObserver *> observers_;

  MessagePumpForIO::FdWatchController controller_;
#if defined(_WIN32)
  struct WinNotifier;
  std::shared_ptr<WinNotifier> win_notifier_;
#else
  int mount_fd_ = -1;
#endif
};

} // namespace nei

#endif // NEIXX_STORAGE_STORAGE_MONITOR_IMPL_H_
