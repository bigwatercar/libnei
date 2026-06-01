#pragma once

#ifndef NEIXX_PROCESS_CHILD_PROCESS_STREAM_PROXY_H_
#define NEIXX_PROCESS_CHILD_PROCESS_STREAM_PROXY_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <neixx/io/async_stream.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/task/task_runner.h>

namespace nei {
namespace internal {

class AsyncInputStreamProxy final : public AsyncInputStream {
 public:
  AsyncInputStreamProxy();
  ~AsyncInputStreamProxy() override;

  void Bind(AsyncInputStream* target, scoped_refptr<TaskRunner> io_task_runner);
  void ResetBinding();

  void ReadAsync(DataCallback callback) override;
  void Close() override;

 private:
  struct State {
    std::mutex lock;
    AsyncInputStream* target = nullptr;
    scoped_refptr<TaskRunner> io_task_runner;
    scoped_refptr<TaskRunner> target_task_runner;
    DataCallback callback;
    bool armed = false;
    bool close_requested = false;
    std::uint64_t binding_generation = 0;
  };

  static void ArmReadOnIoThread(const std::shared_ptr<State>& state,
                                WeakPtr<AsyncInputStreamProxy> weak_self);

  std::shared_ptr<State> state_;
  WeakPtrFactory<AsyncInputStreamProxy> weak_factory_;
};

class AsyncOutputStreamProxy final : public AsyncOutputStream {
 public:
  AsyncOutputStreamProxy();
  ~AsyncOutputStreamProxy() override;

  void Bind(AsyncOutputStream* target, scoped_refptr<TaskRunner> runner);
  void ResetBinding();

  void WriteAsync(std::vector<std::uint8_t> data,
                  WriteCompleteCallback callback) override;
  void Close() override;

 private:
  struct State {
    std::mutex lock;
    AsyncOutputStream* target = nullptr;
    scoped_refptr<TaskRunner> runner;
    bool close_requested = false;
  };

  std::shared_ptr<State> state_;
};

}  // namespace internal
}  // namespace nei

#endif  // NEIXX_PROCESS_CHILD_PROCESS_STREAM_PROXY_H_
