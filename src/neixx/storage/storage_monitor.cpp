#include <neixx/storage/storage_monitor.h>

#include <algorithm>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/task/task_traits.h>
#include <neixx/task/thread_pool_instance.h>

#include "storage_monitor_impl.h"

namespace nei {
namespace {

#if defined(_WIN32)
constexpr char kSep = '\\';

// Windows paths are case-insensitive and accept both separators.
std::string NormalizePathForMatch(const std::string &path) {
  std::string out = path;
  for (char &c : out) {
    if (c == '/')
      c = '\\';
    else if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}
#else
constexpr char kSep = '/';

std::string NormalizePathForMatch(const std::string &path) {
  return path;
}
#endif

} // namespace

// ===========================================================================
// Shell
// ===========================================================================

StorageMonitor::StorageMonitor(scoped_refptr<SingleThreadTaskRunner> task_runner)
    : StorageMonitor(std::move(task_runner), Options()) {
}

StorageMonitor::StorageMonitor(scoped_refptr<SingleThreadTaskRunner> task_runner, Options options)
    : impl_(std::make_unique<Impl>(std::move(task_runner), std::move(options))) {
}

StorageMonitor::~StorageMonitor() = default;

bool StorageMonitor::Start() {
  return impl_->Start();
}

void StorageMonitor::Stop() {
  impl_->Stop();
}

bool StorageMonitor::is_started() const {
  return impl_->is_started();
}

void StorageMonitor::AddObserver(RemovableStorageObserver *observer) {
  impl_->AddObserver(observer);
}

void StorageMonitor::RemoveObserver(RemovableStorageObserver *observer) {
  impl_->RemoveObserver(observer);
}

std::vector<StorageInfo> StorageMonitor::GetAllAvailableStorages() const {
  return impl_->GetAllAvailableStorages();
}

bool StorageMonitor::GetStorageInfoForPath(const std::string &path, StorageInfo *info) const {
  return impl_->GetStorageInfoForPath(path, info);
}

void StorageMonitor::Rescan() {
  impl_->Rescan();
}

void StorageMonitor::EjectDevice(const std::string &device_id, OnceCallback<void(EjectStatus)> callback) {
  impl_->EjectDevice(device_id, std::move(callback));
}

void StorageMonitor::SimulateChangeEventForTesting() {
  impl_->OnPlatformEvent();
}

// ===========================================================================
// Impl (platform independent part)
// ===========================================================================

StorageMonitor::Impl::Impl(scoped_refptr<SingleThreadTaskRunner> task_runner, Options options)
    : task_runner_(std::move(task_runner))
    , options_(std::move(options))
    , scan_config_{options_.mountinfo_path, options_.sysfs_root, options_.dev_disk_root}
    , alive_(std::make_shared<Alive>(Alive{this})) {
  DCHECK(task_runner_ != nullptr);
}

StorageMonitor::Impl::~Impl() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  alive_->impl = nullptr;
  Stop();
}

bool StorageMonitor::Impl::Start() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (started_)
    return true;

  // Enumerate before watching so a change in between is caught by the first
  // event rather than lost.
  ApplySnapshot(PlatformScan(scan_config_), false);
  if (!PlatformStart()) {
    storages_.clear();
    return false;
  }
  if (ThreadPoolInstance *pool = ThreadPoolInstance::Get())
    blocking_runner_ = pool->CreateSequencedTaskRunner(TaskTraits(MayBlock()));
  started_ = true;
  return true;
}

void StorageMonitor::Impl::Stop() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (!started_)
    return;
  PlatformStop();
  started_ = false;
  scan_dirty_ = false;
}

void StorageMonitor::Impl::AddObserver(RemovableStorageObserver *observer) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK(observer != nullptr);
  if (std::find(observers_.begin(), observers_.end(), observer) == observers_.end())
    observers_.push_back(observer);
}

void StorageMonitor::Impl::RemoveObserver(RemovableStorageObserver *observer) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  observers_.erase(std::remove(observers_.begin(), observers_.end(), observer), observers_.end());
}

std::vector<StorageInfo> StorageMonitor::Impl::GetAllAvailableStorages() const {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  std::vector<StorageInfo> result;
  result.reserve(storages_.size());
  for (const auto &kv : storages_)
    result.push_back(kv.second);
  return result;
}

bool StorageMonitor::Impl::GetStorageInfoForPath(const std::string &path, StorageInfo *info) const {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  const std::string key = NormalizePathForMatch(path);
  const StorageInfo *best = nullptr;
  for (const auto &kv : storages_) {
    const std::string loc = NormalizePathForMatch(kv.second.location);
    if (loc.empty() || key.compare(0, loc.size(), loc) != 0)
      continue;
    // The match must end on a path component boundary.
    const bool boundary = loc.back() == kSep || key.size() == loc.size() || key[loc.size()] == kSep;
    if (boundary && (best == nullptr || loc.size() > NormalizePathForMatch(best->location).size()))
      best = &kv.second;
  }
  if (best == nullptr)
    return false;
  if (info != nullptr)
    *info = *best;
  return true;
}

void StorageMonitor::Impl::Rescan() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (!started_)
    return;
  // Invalidate any scan that is already running.  Its snapshot was taken from
  // the mount table as it was *before* this call, so applying it later would
  // revert this result and emit a reverse ATTACHED/DETACHED pair to observers.
  ++scan_generation_;
  ApplySnapshot(PlatformScan(scan_config_), true);
}

void StorageMonitor::Impl::OnFileCanWriteWithoutBlocking(NativeIOHandle) {
  OnPlatformEvent();
}

void StorageMonitor::Impl::OnPlatformEvent() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (!started_)
    return;
  if (blocking_runner_ == nullptr) {
    Rescan();
    RunEventHook();
    return;
  }
  // One scan at a time; events arriving meanwhile collapse into one follow-up.
  if (scan_in_flight_) {
    scan_dirty_ = true;
    return;
  }
  StartAsyncScan();
}

void StorageMonitor::Impl::StartAsyncScan() {
  scan_in_flight_ = true;
  scan_dirty_ = false;
  const std::uint64_t generation = ++scan_generation_;
  // Copied so the worker never touches Impl (monitor-sequence only).
  RepeatingClosure before_completion = options_.before_scan_completion_for_testing;
  blocking_runner_->PostTask(
      FROM_HERE, [alive = alive_, config = scan_config_, runner = task_runner_, generation, before_completion]() {
        std::vector<StorageInfo> result = PlatformScan(config);
        if (before_completion)
          before_completion.Run();
        runner->PostTask(FROM_HERE, [alive, generation, result = std::move(result)]() {
          if (alive->impl != nullptr)
            alive->impl->OnAsyncScanDone(generation, result);
        });
      });
}

void StorageMonitor::Impl::OnAsyncScanDone(std::uint64_t generation, std::vector<StorageInfo> result) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  scan_in_flight_ = false;
  if (!started_) {
    return;
  }

  RepeatingClosure after_done = options_.after_async_scan_done_for_testing;
  if (generation != scan_generation_) {
    // A newer Rescan() (or a newer scan) already published a fresher snapshot;
    // this result predates it.  Drop it, but honor events that arrived while
    // the stale scan was in flight.
    if (scan_dirty_)
      StartAsyncScan();
    if (after_done)
      after_done.Run();
    return;
  }

  ApplySnapshot(std::move(result), true);
  RunEventHook();
  if (scan_dirty_)
    StartAsyncScan();
  if (after_done)
    after_done.Run();
}

void StorageMonitor::Impl::RunEventHook() {
  if (options_.on_event_rescan_for_testing)
    options_.on_event_rescan_for_testing.Run();
}

void StorageMonitor::Impl::EjectDevice(const std::string &device_id, OnceCallback<void(EjectStatus)> callback) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  // Shared so the posted closures stay copyable.
  auto cb = std::make_shared<OnceCallback<void(EjectStatus)>>(std::move(callback));
  const ScanConfig config = scan_config_;

  if (blocking_runner_ == nullptr) {
    std::string location;
    EjectStatus status = ResolveEjectTarget(device_id, config, &location);
    if (status == EjectStatus::kOk)
      status = PlatformEject(location);
    DeliverEjectResult(std::move(cb), status);
    return;
  }

  blocking_runner_->PostTask(FROM_HERE, [alive = alive_, cb, device_id, config, runner = task_runner_]() {
    std::string location;
    EjectStatus status = ResolveEjectTarget(device_id, config, &location);
    if (status == EjectStatus::kOk)
      status = PlatformEject(location);
    runner->PostTask(FROM_HERE, [alive, cb, status]() {
      if (alive->impl != nullptr)
        alive->impl->DeliverEjectResult(cb, status);
    });
  });
}

// The cached snapshot may be stale: the volume could have been unmounted, or
// replaced at the same mount point by another device.  Ejecting a path from
// the snapshot could therefore unmount the wrong volume, so resolve from a
// fresh scan instead.
EjectStatus StorageMonitor::Impl::ResolveEjectTarget(const std::string &device_id,
                                                     const ScanConfig &config,
                                                     std::string *location) {
  for (const StorageInfo &info : PlatformScan(config)) {
    if (info.device_id != device_id)
      continue;
    if (info.type != StorageInfo::Type::kRemovableMassStorage)
      return EjectStatus::kNotRemovable;
    *location = info.location;
    return EjectStatus::kOk;
  }
  return EjectStatus::kNoSuchDevice;
}

// Always bounces through the runner so the callback is never run inline.
void StorageMonitor::Impl::DeliverEjectResult(std::shared_ptr<OnceCallback<void(EjectStatus)>> callback,
                                              EjectStatus status) {
  task_runner_->PostTask(FROM_HERE, [alive = alive_, callback, status]() {
    if (alive->impl != nullptr && *callback)
      std::move(*callback).Run(status);
  });
}

void StorageMonitor::Impl::ApplySnapshot(std::vector<StorageInfo> next, bool notify) {
  // First entry wins when a device is mounted at several places.
  std::map<std::string, StorageInfo> next_map;
  for (auto &info : next)
    next_map.emplace(info.device_id, std::move(info));

  std::vector<StorageInfo> detached;
  std::vector<StorageInfo> attached;
  for (const auto &kv : storages_) {
    auto it = next_map.find(kv.first);
    if (it == next_map.end() || it->second.location != kv.second.location)
      detached.push_back(kv.second);
  }
  for (const auto &kv : next_map) {
    auto it = storages_.find(kv.first);
    if (it == storages_.end() || it->second.location != kv.second.location)
      attached.push_back(kv.second);
  }
  storages_ = std::move(next_map);

  if (!notify)
    return;

  // Iterate a copy and re-check membership so observers may unregister
  // themselves (or others) from inside a callback.
  auto notify_all = [this](const std::vector<StorageInfo> &infos, bool is_attach) {
    for (const auto &info : infos) {
      if (info.type != StorageInfo::Type::kRemovableMassStorage)
        continue;
      const std::vector<RemovableStorageObserver *> snapshot = observers_;
      for (RemovableStorageObserver *obs : snapshot) {
        if (std::find(observers_.begin(), observers_.end(), obs) == observers_.end())
          continue;
        if (is_attach)
          obs->OnRemovableStorageAttached(info);
        else
          obs->OnRemovableStorageDetached(info);
      }
    }
  };
  notify_all(detached, false);
  notify_all(attached, true);
}

} // namespace nei
