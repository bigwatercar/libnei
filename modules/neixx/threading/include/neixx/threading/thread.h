#pragma once

#ifndef NEIXX_THREADING_THREAD_H_
#define NEIXX_THREADING_THREAD_H_

#include <memory>
#include <string>

#include <nei/macros/nei_export.h>
#include <neixx/synchronization/lock.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/platform_thread.h>

namespace nei {

class NEI_API Thread final : public PlatformThread::Delegate {
 public:
  explicit Thread(const std::string& name = std::string());
  ~Thread() override;

  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;
  Thread(Thread&&) = delete;
  Thread& operator=(Thread&&) = delete;

  bool Start();
  void Stop();

  scoped_refptr<TaskRunner> GetTaskRunner() const;
  bool IsRunning() const;

 private:
  void ThreadMain() override;

  std::string name_;
  mutable Lock lock_;
  PlatformThread::Handle handle_;
  scoped_refptr<TaskRunner> task_runner_;
  std::unique_ptr<WaitableEvent> start_event_;
  bool started_ = false;
  bool running_ = false;
  bool start_succeeded_ = false;
};

}  // namespace nei

#endif  // NEIXX_THREADING_THREAD_H_
