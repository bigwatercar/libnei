#pragma once

#ifndef NEIXX_PROCESS_CHILD_PROCESS_STREAM_PROXY_H_
#define NEIXX_PROCESS_CHILD_PROCESS_STREAM_PROXY_H_

#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/io/async_stream.h>
#include <neixx/task/task_runner.h>

namespace nei {
namespace internal {

class AsyncInputStreamProxy final : public AsyncInputStream {
 public:
  AsyncInputStreamProxy() = default;
  ~AsyncInputStreamProxy() override { Close(); }

  void Bind(AsyncInputStream* target, scoped_refptr<TaskRunner> runner) {
    bool should_arm = false;
    {
      std::lock_guard<std::mutex> lock(lock_);
      target_ = target;
      runner_ = std::move(runner);
      if (close_requested_) {
        should_arm = false;
      } else if (callback_ && target_ != nullptr && !armed_) {
        armed_ = true;
        should_arm = true;
      }
    }

    if (close_requested_) {
      Close();
      return;
    }
    if (should_arm) {
      ArmReadOnIoThread();
    }
  }

  void ResetBinding() {
    std::lock_guard<std::mutex> lock(lock_);
    target_ = nullptr;
    runner_.reset();
    armed_ = false;
  }

  void ReadAsync(DataCallback callback) override {
    bool should_arm = false;
    {
      std::lock_guard<std::mutex> lock(lock_);
      callback_ = std::move(callback);
      if (!close_requested_ && target_ != nullptr && !armed_) {
        armed_ = true;
        should_arm = true;
      }
    }

    if (should_arm) {
      ArmReadOnIoThread();
    }
  }

  void Close() override {
    scoped_refptr<TaskRunner> runner;
    {
      std::lock_guard<std::mutex> lock(lock_);
      if (close_requested_) {
        return;
      }
      close_requested_ = true;
      runner = runner_;
    }

    if (runner.get() != nullptr) {
      runner->PostTask(FROM_HERE, [this]() {
        AsyncInputStream* target = nullptr;
        {
          std::lock_guard<std::mutex> lock(lock_);
          target = target_;
          target_ = nullptr;
          armed_ = false;
          runner_.reset();
        }
        if (target != nullptr) {
          target->Close();
        }
      });
    }
  }

 private:
  void ArmReadOnIoThread() {
    scoped_refptr<TaskRunner> runner;
    {
      std::lock_guard<std::mutex> lock(lock_);
      runner = runner_;
    }
    if (runner.get() == nullptr) {
      return;
    }

    runner->PostTask(FROM_HERE, [this]() {
      AsyncInputStream* target = nullptr;
      {
        std::lock_guard<std::mutex> lock(lock_);
        target = target_;
        if (close_requested_ || target == nullptr || !callback_) {
          armed_ = false;
          return;
        }
      }

      target->ReadAsync([this](std::vector<std::uint8_t>&& data) {
        DataCallback callback;
        {
          std::lock_guard<std::mutex> lock(lock_);
          callback = callback_;
          if (data.empty()) {
            target_ = nullptr;
            runner_.reset();
            armed_ = false;
          }
        }
        if (callback) {
          callback(std::move(data));
        }
      });
    });
  }

  std::mutex lock_;
  AsyncInputStream* target_ = nullptr;
  scoped_refptr<TaskRunner> runner_;
  DataCallback callback_;
  bool armed_ = false;
  bool close_requested_ = false;
};

class AsyncOutputStreamProxy final : public AsyncOutputStream {
 public:
  AsyncOutputStreamProxy() = default;
  ~AsyncOutputStreamProxy() override { Close(); }

  void Bind(AsyncOutputStream* target, scoped_refptr<TaskRunner> runner) {
    bool should_close = false;
    {
      std::lock_guard<std::mutex> lock(lock_);
      target_ = target;
      runner_ = std::move(runner);
      should_close = close_requested_ && target_ != nullptr;
    }

    if (should_close) {
      Close();
    }
  }

  void ResetBinding() {
    std::lock_guard<std::mutex> lock(lock_);
    target_ = nullptr;
    runner_.reset();
  }

  void WriteAsync(std::vector<std::uint8_t> data,
                  WriteCompleteCallback callback) override {
    scoped_refptr<TaskRunner> runner;
    {
      std::lock_guard<std::mutex> lock(lock_);
      if (close_requested_ || target_ == nullptr) {
        if (callback) {
          callback(false);
        }
        return;
      }
      runner = runner_;
    }

    if (runner.get() == nullptr) {
      if (callback) {
        callback(false);
      }
      return;
    }

    runner->PostTask(FROM_HERE,
                     [this, data = std::move(data), callback = std::move(callback)]() mutable {
      AsyncOutputStream* target = nullptr;
      {
        std::lock_guard<std::mutex> lock(lock_);
        if (close_requested_) {
          target = nullptr;
        } else {
          target = target_;
        }
      }
      if (target == nullptr) {
        if (callback) {
          callback(false);
        }
        return;
      }
      target->WriteAsync(std::move(data), std::move(callback));
    });
  }

  void Close() override {
    scoped_refptr<TaskRunner> runner;
    {
      std::lock_guard<std::mutex> lock(lock_);
      if (close_requested_) {
        return;
      }
      close_requested_ = true;
      runner = runner_;
    }

    if (runner.get() != nullptr) {
      runner->PostTask(FROM_HERE, [this]() {
        AsyncOutputStream* target = nullptr;
        {
          std::lock_guard<std::mutex> lock(lock_);
          target = target_;
          target_ = nullptr;
          runner_.reset();
        }
        if (target != nullptr) {
          target->Close();
        }
      });
    }
  }

 private:
  std::mutex lock_;
  AsyncOutputStream* target_ = nullptr;
  scoped_refptr<TaskRunner> runner_;
  bool close_requested_ = false;
};

}  // namespace internal
}  // namespace nei

#endif  // NEIXX_PROCESS_CHILD_PROCESS_STREAM_PROXY_H_
