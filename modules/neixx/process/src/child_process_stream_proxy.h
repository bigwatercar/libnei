#pragma once

#ifndef NEIXX_PROCESS_CHILD_PROCESS_STREAM_PROXY_H_
#define NEIXX_PROCESS_CHILD_PROCESS_STREAM_PROXY_H_

#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/io/async_stream.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/task/task_runner.h>

namespace nei {
namespace internal {

class AsyncInputStreamProxy final : public AsyncInputStream {
 public:
  AsyncInputStreamProxy()
      : state_(std::make_shared<State>()), weak_factory_(this) {}
  ~AsyncInputStreamProxy() override { Close(); }

  void Bind(AsyncInputStream* target,
            scoped_refptr<TaskRunner> io_task_runner) {
    bool should_arm = false;
    bool should_close = false;
    {
      std::lock_guard<std::mutex> lock(state_->lock);
      state_->binding_generation++;
      state_->target = target;
      state_->io_task_runner = std::move(io_task_runner);
      state_->target_task_runner = state_->io_task_runner;
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
      ArmReadOnIoThread(state_, weak_factory_.GetWeakPtr());
    }
  }

  void ResetBinding() {
    std::lock_guard<std::mutex> lock(state_->lock);
    state_->binding_generation++;
    state_->target = nullptr;
    state_->io_task_runner.reset();
    state_->target_task_runner.reset();
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
      ArmReadOnIoThread(state_, weak_factory_.GetWeakPtr());
    }
  }

  void Close() override {
    scoped_refptr<TaskRunner> io_task_runner;
    {
      std::lock_guard<std::mutex> lock(state_->lock);
      if (state_->close_requested) {
        return;
      }
      state_->binding_generation++;
      state_->close_requested = true;
      io_task_runner = state_->io_task_runner;
    }

    // Invalidate every already-posted target-sequence task. Any in-flight IO
    // completion that bounces back after this point will observe null WeakPtr.
    weak_factory_.InvalidateWeakPtrs();

    if (io_task_runner.get() != nullptr) {
      const std::shared_ptr<State> state = state_;
      io_task_runner->PostTask(FROM_HERE, [state]() {
        AsyncInputStream* target = nullptr;
        {
          std::lock_guard<std::mutex> lock(state->lock);
          target = state->target;
          state->target = nullptr;
          state->armed = false;
          state->io_task_runner.reset();
          state->target_task_runner.reset();
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
    scoped_refptr<TaskRunner> io_task_runner;
    scoped_refptr<TaskRunner> target_task_runner;
    DataCallback callback;
    bool armed = false;
    bool close_requested = false;
    std::uint64_t binding_generation = 0;
  };

  static void ArmReadOnIoThread(const std::shared_ptr<State>& state,
                                WeakPtr<AsyncInputStreamProxy> weak_self) {
    scoped_refptr<TaskRunner> io_task_runner;
    {
      std::lock_guard<std::mutex> lock(state->lock);
      io_task_runner = state->io_task_runner;
    }
    if (io_task_runner.get() == nullptr) {
      return;
    }

    io_task_runner->PostTask(FROM_HERE, [state, weak_self]() {
      if (!weak_self) {
        return;
      }
      AsyncInputStream* target = nullptr;
      std::uint64_t generation = 0;
      {
        std::lock_guard<std::mutex> lock(state->lock);
        target = state->target;
        if (state->close_requested || target == nullptr || !state->callback) {
          state->armed = false;
          return;
        }
        generation = state->binding_generation;
      }

      target->ReadAsync([state, weak_self, generation](std::vector<std::uint8_t>&& data) {
        scoped_refptr<TaskRunner> target_runner;
        scoped_refptr<TaskRunner> io_runner;
        DataCallback callback;
        bool drop = false;
        {
          std::lock_guard<std::mutex> lock(state->lock);
          if (state->close_requested || state->binding_generation != generation) {
            drop = true;
          }
          io_runner = state->io_task_runner;
          target_runner = state->target_task_runner;
          callback = state->callback;
          if (data.empty()) {
            state->target = nullptr;
            state->io_task_runner.reset();
            state->target_task_runner.reset();
            state->armed = false;
            state->binding_generation++;
          }
        }
        if (drop || !callback || target_runner.get() == nullptr) {
          return;
        }

        if (target_runner.get() == io_runner.get()) {
          if (!weak_self) {
            return;
          }
          callback(std::move(data));
          return;
        }

        // Architecture contract:
        // 1) Windows IOCP or POSIX epoll/kqueue callback thread only captures
        //    completion bytes and snapshots callback context.
        // 2) Business callback execution is *always* bounced to target runner.
        // 3) WeakPtr + generation guard prevents UAF and suppresses stale
        //    completions when unbind/close races with in-flight IO.
        target_runner->PostTask(
            FROM_HERE,
            [weak_self, callback = std::move(callback),
             data = std::move(data)]() mutable {
              if (!weak_self) {
                return;
              }
              callback(std::move(data));
            });
      });
    });
  }

  std::shared_ptr<State> state_;
  WeakPtrFactory<AsyncInputStreamProxy> weak_factory_;
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
