#pragma once

#ifndef NEIXX_PROCESS_CHILD_PROCESS_STREAM_PROXY_H_
#define NEIXX_PROCESS_CHILD_PROCESS_STREAM_PROXY_H_

#include <functional>
#include <memory>
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
  AsyncInputStreamProxy() : state_(std::make_shared<State>()) {}
  ~AsyncInputStreamProxy() override { Close(); }

  void Bind(AsyncInputStream* target, scoped_refptr<TaskRunner> runner) {
    bool should_arm = false;
    bool should_close = false;
    {
      std::lock_guard<std::mutex> lock(state_->lock);
      state_->target = target;
      state_->runner = std::move(runner);
      if (state_->close_requested) {
        should_close = target != nullptr;
      } else if (state_->callback && state_->target != nullptr && !state_->armed) {
        state_->armed = true;
        should_arm = true;
      }
    }

    if (should_close) {
      Close();
      return;
    }
    if (should_arm) {
      ArmReadOnIoThread(state_);
    }
  }

  void ResetBinding() {
    std::lock_guard<std::mutex> lock(state_->lock);
    state_->target = nullptr;
    state_->runner.reset();
    state_->armed = false;
  }

  void ReadAsync(DataCallback callback) override {
    bool should_arm = false;
    {
      std::lock_guard<std::mutex> lock(state_->lock);
      state_->callback = std::move(callback);
      if (!state_->close_requested && state_->target != nullptr && !state_->armed) {
        state_->armed = true;
        should_arm = true;
      }
    }

    if (should_arm) {
      ArmReadOnIoThread(state_);
    }
  }

  void Close() override {
    scoped_refptr<TaskRunner> runner;
    {
      std::lock_guard<std::mutex> lock(state_->lock);
      if (state_->close_requested) {
        return;
      }
      state_->close_requested = true;
      runner = state_->runner;
    }

    if (runner.get() != nullptr) {
      const std::shared_ptr<State> state = state_;
      runner->PostTask(FROM_HERE, [state]() {
        AsyncInputStream* target = nullptr;
        {
          std::lock_guard<std::mutex> lock(state->lock);
          target = state->target;
          state->target = nullptr;
          state->armed = false;
          state->runner.reset();
        }
        if (target != nullptr) {
          target->Close();
        }
      });
    }
  }

 private:
    struct State {
      std::mutex lock;
      AsyncInputStream* target = nullptr;
      scoped_refptr<TaskRunner> runner;
      DataCallback callback;
      bool armed = false;
      bool close_requested = false;
    };

    static void ArmReadOnIoThread(const std::shared_ptr<State>& state) {
    scoped_refptr<TaskRunner> runner;
    {
        std::lock_guard<std::mutex> lock(state->lock);
        runner = state->runner;
    }
    if (runner.get() == nullptr) {
      return;
    }

      runner->PostTask(FROM_HERE, [state]() {
      AsyncInputStream* target = nullptr;
      {
          std::lock_guard<std::mutex> lock(state->lock);
          target = state->target;
          if (state->close_requested || target == nullptr || !state->callback) {
            state->armed = false;
          return;
        }
      }

        target->ReadAsync([state](std::vector<std::uint8_t>&& data) {
        DataCallback callback;
        {
            std::lock_guard<std::mutex> lock(state->lock);
            callback = state->callback;
          if (data.empty()) {
              state->target = nullptr;
              state->runner.reset();
              state->armed = false;
          }
        }
        if (callback) {
          callback(std::move(data));
        }
      });
    });
  }

  std::shared_ptr<State> state_;
};

class AsyncOutputStreamProxy final : public AsyncOutputStream {
 public:
  AsyncOutputStreamProxy() : state_(std::make_shared<State>()) {}
  ~AsyncOutputStreamProxy() override { Close(); }

  void Bind(AsyncOutputStream* target, scoped_refptr<TaskRunner> runner) {
    bool should_close = false;
    {
      std::lock_guard<std::mutex> lock(state_->lock);
      state_->target = target;
      state_->runner = std::move(runner);
      should_close = state_->close_requested && state_->target != nullptr;
    }

    if (should_close) {
      Close();
    }
  }

  void ResetBinding() {
    std::lock_guard<std::mutex> lock(state_->lock);
    state_->target = nullptr;
    state_->runner.reset();
  }

  void WriteAsync(std::vector<std::uint8_t> data,
                  WriteCompleteCallback callback) override {
    scoped_refptr<TaskRunner> runner;
    {
      std::lock_guard<std::mutex> lock(state_->lock);
      if (state_->close_requested || state_->target == nullptr) {
        if (callback) {
          callback(false);
        }
        return;
      }
      runner = state_->runner;
    }

    if (runner.get() == nullptr) {
      if (callback) {
        callback(false);
      }
      return;
    }

    const std::shared_ptr<State> state = state_;
    runner->PostTask(FROM_HERE,
                     [state, data = std::move(data), callback = std::move(callback)]() mutable {
      AsyncOutputStream* target = nullptr;
      {
        std::lock_guard<std::mutex> lock(state->lock);
        if (state->close_requested) {
          target = nullptr;
        } else {
          target = state->target;
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
      std::lock_guard<std::mutex> lock(state_->lock);
      if (state_->close_requested) {
        return;
      }
      state_->close_requested = true;
      runner = state_->runner;
    }

    if (runner.get() != nullptr) {
      const std::shared_ptr<State> state = state_;
      runner->PostTask(FROM_HERE, [state]() {
        AsyncOutputStream* target = nullptr;
        {
          std::lock_guard<std::mutex> lock(state->lock);
          target = state->target;
          state->target = nullptr;
          state->runner.reset();
        }
        if (target != nullptr) {
          target->Close();
        }
      });
    }
  }

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
