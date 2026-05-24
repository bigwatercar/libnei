#pragma once

#ifndef NEIXX_PROCESS_PROCESS_SERVICE_H_
#define NEIXX_PROCESS_PROCESS_SERVICE_H_

#include <memory>
#include <string>

#include <nei/macros/nei_export.h>
#include <neixx/memory/ref_counted.h>

namespace nei {

class TaskRunner;

class NEI_API ProcessService final
    : public RefCountedThreadSafe<ProcessService> {
 public:
  static scoped_refptr<ProcessService> Create(
      const std::string& thread_name = "process-service-io");
  static scoped_refptr<ProcessService> GetDefault();

  bool Start();
  bool IsRunning() const;
  bool IsOnServiceThread() const;
  scoped_refptr<TaskRunner> GetTaskRunner() const;

 private:
    template <typename T, typename... Args>
    friend scoped_refptr<T> MakeRefCounted(Args&&... args);
  friend class RefCountedThreadSafe<ProcessService>;

  class Impl;

  explicit ProcessService(const std::string& thread_name);
  ~ProcessService();

  std::unique_ptr<Impl> impl_;
};

}  // namespace nei

#endif  // NEIXX_PROCESS_PROCESS_SERVICE_H_
