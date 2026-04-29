#ifndef NEIXX_THREADING_THREAD_ID_NAME_MANAGER_H_
#define NEIXX_THREADING_THREAD_ID_NAME_MANAGER_H_

#include <shared_mutex>
#include <string>
#include <unordered_map>

#include <nei/macros/nei_export.h>
#include <neixx/threading/platform_thread.h>

namespace nei {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

class NEI_API ThreadIdNameManager final {
public:
  ThreadIdNameManager(const ThreadIdNameManager &) = delete;
  ThreadIdNameManager &operator=(const ThreadIdNameManager &) = delete;

  static ThreadIdNameManager *GetInstance();

  void RegisterThread(PlatformThreadId id, const std::string &name);
  void RemoveThread(PlatformThreadId id);
  std::string GetName(PlatformThreadId id) const;

private:
  ThreadIdNameManager() = default;

  mutable std::shared_mutex mutex_;
  std::unordered_map<PlatformThreadId, std::string> thread_names_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace nei

#endif // NEIXX_THREADING_THREAD_ID_NAME_MANAGER_H_
