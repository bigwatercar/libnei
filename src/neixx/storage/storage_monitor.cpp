#include <neixx/storage/storage_monitor.h>

#include <algorithm>

#include <nei/debug/check.h>

#include "storage_monitor_impl.h"

namespace nei {

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

// ===========================================================================
// Impl (platform independent part)
// ===========================================================================

StorageMonitor::Impl::Impl(scoped_refptr<SingleThreadTaskRunner> task_runner, Options options)
    : task_runner_(std::move(task_runner))
    , options_(std::move(options)) {
  DCHECK(task_runner_ != nullptr);
}

StorageMonitor::Impl::~Impl() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  Stop();
}

bool StorageMonitor::Impl::Start() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (started_)
    return true;

  // Enumerate before watching so a change in between is caught by the first
  // event rather than lost.
  ApplySnapshot(PlatformScan(), false);
  if (!PlatformStart()) {
    storages_.clear();
    return false;
  }
  started_ = true;
  return true;
}

void StorageMonitor::Impl::Stop() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (!started_)
    return;
  PlatformStop();
  started_ = false;
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
  const StorageInfo *best = nullptr;
  for (const auto &kv : storages_) {
    const std::string &loc = kv.second.location;
    if (loc.empty() || path.compare(0, loc.size(), loc) != 0)
      continue;
    // The match must end on a path component boundary.
    const bool boundary = loc.back() == '/' || path.size() == loc.size() || path[loc.size()] == '/';
    if (boundary && (best == nullptr || loc.size() > best->location.size()))
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
  ApplySnapshot(PlatformScan(), true);
}

void StorageMonitor::Impl::OnFileCanWriteWithoutBlocking(NativeIOHandle) {
  Rescan();
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
