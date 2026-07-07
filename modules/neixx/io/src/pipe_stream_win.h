#pragma once

#ifndef NEIXX_IO_PIPE_STREAM_WIN_H_
#define NEIXX_IO_PIPE_STREAM_WIN_H_

#if defined(_WIN32)

#include <windows.h>

#include <cstddef>
#include <deque>
#include <memory>

#include <neixx/common/location.h>
#include <neixx/functional/bind.h>
#include <neixx/io/io_buffer.h>
#include <neixx/io/pipe_stream.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/task/bind_post_task.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/task_runner.h>
#include <neixx/trace_event/trace_event.h>

namespace nei {
namespace pipe_detail {

template <typename Callback>
void PostError(const scoped_refptr<TaskRunner>& runner, Callback&& cb) {
  if (!cb) return;
  if (runner) {
    BindPostTask(runner,
                 BindOnce([](Callback c) { c(false, 0u); },
                          std::forward<Callback>(cb)))
        .Run();
  } else {
    cb(false, 0u);
  }
}

template <typename Callback>
void PostResult(const scoped_refptr<TaskRunner>& runner,
                Callback&& cb,
                bool success,
                std::size_t bytes) {
  if (!cb) return;
  if (runner) {
    BindPostTask(runner,
                 BindOnce([](Callback c, bool s, std::size_t n) { c(s, n); },
                          std::forward<Callback>(cb), success, bytes))
        .Run();
  } else {
    cb(success, bytes);
  }
}

}  // namespace pipe_detail

template <>
struct WeakPtrThreadSafe<PipeInputStream::Impl> : std::true_type {};
template <>
struct WeakPtrThreadSafe<PipeOutputStream::Impl> : std::true_type {};

struct ReadContext {
  OVERLAPPED overlapped = {};
  scoped_refptr<IOBuffer> buffer;
  AsyncInputStream::IOReadCallback callback;
  HANDLE io_event = nullptr;

  ReadContext() { io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
  ~ReadContext() { if (io_event) CloseHandle(io_event); }
};

struct WriteContext {
  OVERLAPPED overlapped = {};
  scoped_refptr<IOBuffer> buffer;
  std::size_t buf_len = 0;
  AsyncOutputStream::IOWriteCallback callback;
  HANDLE io_event = nullptr;

  WriteContext() { io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
  ~WriteContext() { if (io_event) CloseHandle(io_event); }
};

// ===========================================================================
// PipeInputStream::Impl
// ===========================================================================

class PipeInputStream::Impl final
    : public MessagePumpForIO::CompletionWatcher {
 public:
  explicit Impl(scoped_refptr<TaskRunner> io_task_runner);
  ~Impl() override;

  bool BindPlatformHandle(PlatformHandle handle);

  void ReadAsync(scoped_refptr<IOBuffer> buf,
                 std::size_t buf_len,
                 IOReadCallback callback);

  void Close();

  void OnIOCompleted(NativeIOHandle handle,
                     void* overlapped_context,
                     std::uint32_t bytes_transferred,
                     std::uint32_t error_code) override;

  void OnFileCanReadWithoutBlocking(NativeIOHandle) override {}
  void OnFileCanWriteWithoutBlocking(NativeIOHandle) override {}

  void ShutdownAndSelfDestruct();

  scoped_refptr<TaskRunner> io_task_runner() const { return io_task_runner_; }

 private:
  void IssueRead(std::size_t buf_len);
  void MaybeCloseHandle();

  scoped_refptr<TaskRunner> io_task_runner_;
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  bool closed_ = false;
  bool shutting_down_ = false;

  std::shared_ptr<ReadContext> read_ctx_;
  std::shared_ptr<ReadContext> orphaned_ctx_;

  MessagePumpForIO::FdWatchController controller_;
  WeakPtrFactory<Impl> weak_factory_;
};

// ===========================================================================
// PipeOutputStream::Impl
// ===========================================================================

class PipeOutputStream::Impl final
    : public MessagePumpForIO::CompletionWatcher {
 public:
  explicit Impl(scoped_refptr<TaskRunner> io_task_runner);
  ~Impl() override;

  bool BindPlatformHandle(PlatformHandle handle);

  void WriteAsync(scoped_refptr<IOBuffer> buf,
                  std::size_t buf_len,
                  IOWriteCallback callback);

  void Close();

  void OnIOCompleted(NativeIOHandle handle,
                     void* overlapped_context,
                     std::uint32_t bytes_transferred,
                     std::uint32_t error_code) override;

  void MaybeStartNextQueuedWrite();

  void OnFileCanReadWithoutBlocking(NativeIOHandle) override {}
  void OnFileCanWriteWithoutBlocking(NativeIOHandle) override {}

  void ShutdownAndSelfDestruct();

  scoped_refptr<TaskRunner> io_task_runner() const { return io_task_runner_; }

 private:
  void IssueWrite(std::size_t buf_len);
  void MaybeCloseHandle();

  scoped_refptr<TaskRunner> io_task_runner_;
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  bool closed_ = false;
  bool shutting_down_ = false;

  std::shared_ptr<WriteContext> write_ctx_;
  std::shared_ptr<WriteContext> orphaned_ctx_;
  std::deque<std::shared_ptr<WriteContext>> write_queue_;

  MessagePumpForIO::FdWatchController controller_;
  WeakPtrFactory<Impl> weak_factory_;
};

}  // namespace nei

#endif  // defined(_WIN32)
#endif  // NEIXX_IO_PIPE_STREAM_WIN_H_
