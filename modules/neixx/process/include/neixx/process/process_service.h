#pragma once

#ifndef NEIXX_PROCESS_PROCESS_SERVICE_H_
#define NEIXX_PROCESS_PROCESS_SERVICE_H_

#include <memory>
#include <string>

#include <nei/macros/nei_export.h>
#include <nei/macros/suppress_compiler_warnings.h>
#include <neixx/memory/ref_counted.h>

#include <neixx/task/task_runner.h>

namespace nei {

class NEI_API ProcessService final : public RefCountedThreadSafe<ProcessService> {
public:
  static scoped_refptr<ProcessService> Create(const std::string &thread_name = "process-service-io");
  static scoped_refptr<ProcessService> GetDefault();

  bool Start();
  bool IsRunning() const;
  bool IsOnServiceThread() const;
  scoped_refptr<SingleThreadTaskRunner> GetTaskRunner() const;

private:
  template <typename T, typename... Args>
  friend scoped_refptr<T> MakeRefCounted(Args &&...args);
  friend class RefCountedThreadSafe<ProcessService>;

  class Impl;

  explicit ProcessService(const std::string &thread_name);
  ~ProcessService();

  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_END
};

} // namespace nei

#endif // NEIXX_PROCESS_PROCESS_SERVICE_H_
