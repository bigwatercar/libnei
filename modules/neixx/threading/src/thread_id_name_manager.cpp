#include <neixx/threading/thread_id_name_manager.h>

#include <mutex>
#include <sstream>

namespace nei {

ThreadIdNameManager *ThreadIdNameManager::GetInstance() {
  static ThreadIdNameManager instance;
  return &instance;
}

void ThreadIdNameManager::RegisterThread(PlatformThreadId id, const std::string &name) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  thread_names_[id] = name;
}

void ThreadIdNameManager::RemoveThread(PlatformThreadId id) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  thread_names_.erase(id);
}

std::string ThreadIdNameManager::GetName(PlatformThreadId id) const {
  {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = thread_names_.find(id);
    if (it != thread_names_.end()) {
      return it->second;
    }
  }

  std::ostringstream os;
  os << "Thread:[" << id << "]";
  return os.str();
}

} // namespace nei
