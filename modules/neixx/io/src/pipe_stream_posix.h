#pragma once

#ifndef NEIXX_IO_PIPE_STREAM_POSIX_H_
#define NEIXX_IO_PIPE_STREAM_POSIX_H_

#if !defined(_WIN32)

#include <cstddef>
#include <deque>

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

constexpr std::size_t kMaxBytesPerDrain = 64 * 1024; // 64 KiB

template <typename Callback>
void PostError(const scoped_refptr<SequencedTaskRunner> &runner, Callback &&cb) {
  if (!cb)
    return;
  if (runner) {
    BindPostTask(runner, BindOnce([](Callback c) { c(false, 0u); }, std::forward<Callback>(cb))).Run();
  } else {
    cb(false, 0u);
  }
}

template <typename Callback>
void PostResult(const scoped_refptr<SequencedTaskRunner> &runner, Callback &&cb, bool success, std::size_t bytes) {
  if (!cb)
    return;
  if (runner) {
    BindPostTask(
        runner,
        BindOnce([](Callback c, bool s, std::size_t n) { c(s, n); }, std::forward<Callback>(cb), success, bytes))
        .Run();
  } else {
    cb(success, bytes);
  }
}

} // namespace pipe_detail

template <>
struct WeakPtrThreadSafe<PipeInputStream::Impl> : std::true_type {};

template <>
struct WeakPtrThreadSafe<PipeOutputStream::Impl> : std::true_type {};

// ===========================================================================
// PipeInputStream::Impl
// ===========================================================================

class PipeInputStream::Impl final : public MessagePumpForIO::Watcher {
public:
  explicit Impl(scoped_refptr<SingleThreadTaskRunner> io_task_runner);
  ~Impl() override;

  bool BindPlatformHandle(PlatformHandle handle);

  void ReadAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOReadCallback callback);

  void Close();

  void OnFileCanReadWithoutBlocking(NativeIOHandle /*handle*/) override;

  void OnFileCanWriteWithoutBlocking(NativeIOHandle /*handle*/) override {
  }

  void ShutdownAndSelfDestruct();

  scoped_refptr<SingleThreadTaskRunner> io_task_runner() const {
    return io_task_runner_;
  }

private:
  void DrainRead();
  void DeliverReadResult(bool success, std::size_t bytes);

  scoped_refptr<SingleThreadTaskRunner> io_task_runner_;
  int fd_ = -1;
  bool closed_ = false;
  bool read_in_flight_ = false;
  bool called_from_pump_ = false;

  scoped_refptr<IOBuffer> pending_buf_;
  std::size_t pending_len_ = 0;
  std::size_t bytes_read_ = 0;
  IOReadCallback pending_cb_;

  MessagePumpForIO::FdWatchController controller_;
  WeakPtrFactory<Impl> weak_factory_;
};

// ===========================================================================
// PipeOutputStream::Impl
// ===========================================================================

class PipeOutputStream::Impl final : public MessagePumpForIO::Watcher {
public:
  explicit Impl(scoped_refptr<SingleThreadTaskRunner> io_task_runner);
  ~Impl() override;

  bool BindPlatformHandle(PlatformHandle handle);

  void WriteAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOWriteCallback callback);

  void Close();

  void OnFileCanWriteWithoutBlocking(NativeIOHandle handle) override;

  void OnFileCanReadWithoutBlocking(NativeIOHandle /*handle*/) override {
  }

  void ShutdownAndSelfDestruct();

  scoped_refptr<SingleThreadTaskRunner> io_task_runner() const {
    return io_task_runner_;
  }

private:
  void DrainWrite();
  void DeliverWriteResult(bool success, std::size_t bytes);
  void StartNextQueuedWrite();

  struct PendingWrite {
    scoped_refptr<IOBuffer> buf;
    std::size_t buf_len = 0;
    IOWriteCallback callback;
  };

  scoped_refptr<SingleThreadTaskRunner> io_task_runner_;
  int fd_ = -1;
  bool closed_ = false;
  bool write_in_flight_ = false;
  bool called_from_pump_ = false;

  scoped_refptr<IOBuffer> pending_buf_;
  std::size_t pending_len_ = 0;
  std::size_t bytes_written_ = 0;
  IOWriteCallback pending_cb_;

  std::deque<PendingWrite> write_queue_;

  MessagePumpForIO::FdWatchController controller_;
  WeakPtrFactory<Impl> weak_factory_;
};

} // namespace nei

#endif // !defined(_WIN32)
#endif // NEIXX_IO_PIPE_STREAM_POSIX_H_
