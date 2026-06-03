#if !defined(_WIN32)

#include "internal/pipe_stream_factory_internal.h"

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>

#include <unistd.h>

#include <neixx/io/io_buffer.h>
#include <neixx/task/message_loop/message_pump_io.h>

namespace nei {
namespace {

std::mutex& SigPipeMutex() {
  static std::mutex mutex;
  return mutex;
}

void IgnoreSigPipeOnce() {
  static bool initialized = false;
  std::lock_guard<std::mutex> lock(SigPipeMutex());
  if (initialized) return;
  (void)signal(SIGPIPE, SIG_IGN);
  initialized = true;
}

// ---------------------------------------------------------------------------
// PosixPipeInputStream
//
// Pull model: one ReadAsync() -> one read() -> one callback.
// The caller-supplied IOBuffer is the read() destination; no copy is needed.
// A new ReadAsync() must be issued for each chunk wanted.
// ---------------------------------------------------------------------------
class PosixPipeInputStream final : public AsyncInputStream,
                                   public MessagePumpForIO::Watcher {
 public:
  PosixPipeInputStream(MessagePumpForIO* pump, int fd)
      : pump_(pump), fd_(fd) {}

  ~PosixPipeInputStream() override { Close(); }

  void ReadAsync(scoped_refptr<IOBuffer> buf,
                 std::size_t buf_len,
                 IOReadCallback callback) override {
    if (closed_) {
      if (callback) callback(false, 0u);
      return;
    }
    if (pending_cb_) {
      // Overlapping reads not supported.
      if (callback) callback(false, 0u);
      return;
    }

    pending_buf_ = std::move(buf);
    pending_len_ = buf_len;
    pending_cb_ = std::move(callback);

    if (!controller_.is_watching()) {
      (void)controller_.StartWatching(
          pump_, fd_, MessagePumpForIO::FdWatchController::Mode::READ, this);
    }

    // Attempt an immediate non-blocking read.
    TryRead();
  }

  void Close() override {
    if (closed_) return;
    closed_ = true;
    controller_.StopWatching();
    if (fd_ >= 0) {
      (void)close(fd_);
      fd_ = -1;
    }
    if (pending_cb_) {
      IOReadCallback cb = std::move(pending_cb_);
      pending_buf_.reset();
      cb(false, 0u);
    }
  }

  void OnFileCanReadWithoutBlocking(NativeIOHandle handle) override {
    if (closed_ || !pending_cb_ || static_cast<int>(handle) != fd_) return;
    TryRead();
  }

  void OnFileCanWriteWithoutBlocking(NativeIOHandle /*handle*/) override {}

 private:
  void TryRead() {
    for (;;) {
      const ssize_t n =
          read(fd_, pending_buf_->data(), pending_len_);
      if (n > 0) {
        DeliverSuccess(static_cast<std::size_t>(n));
        return;
      }
      if (n == 0) {
        // EOF.
        DeliverEof();
        return;
      }
      // n < 0
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Not ready; wait for epoll notification.
        return;
      }
      DeliverEof();
      return;
    }
  }

  void DeliverSuccess(std::size_t bytes) {
    IOReadCallback cb = std::move(pending_cb_);
    pending_buf_.reset();
    // Stop watching; the next ReadAsync() will re-enable if needed.
    controller_.StopWatching();
    if (cb) cb(true, bytes);
  }

  void DeliverEof() {
    IOReadCallback cb = std::move(pending_cb_);
    pending_buf_.reset();
    controller_.StopWatching();
    if (cb) cb(false, 0u);
  }

  MessagePumpForIO* pump_ = nullptr;
  int fd_ = -1;
  bool closed_ = false;

  scoped_refptr<IOBuffer> pending_buf_;
  std::size_t pending_len_ = 0;
  IOReadCallback pending_cb_;

  MessagePumpForIO::FdWatchController controller_;
};

// ---------------------------------------------------------------------------
// PosixPipeOutputStream
//
// Each WriteAsync() queues a PendingWrite with a scoped_refptr<IOBuffer>.
// write() is called directly on buf->data() + offset, so no extra copy.
// Multiple queued writes are serialised through the deque; each PendingWrite
// completes atomically (full write before advancing to next entry).
// ---------------------------------------------------------------------------
class PosixPipeOutputStream final : public AsyncOutputStream,
                                    public MessagePumpForIO::Watcher {
 public:
  struct PendingWrite {
    scoped_refptr<IOBuffer> buf;
    std::size_t buf_len = 0;
    std::size_t offset = 0;
    IOWriteCallback callback;
  };

  PosixPipeOutputStream(MessagePumpForIO* pump, int fd)
      : pump_(pump), fd_(fd) {
    IgnoreSigPipeOnce();
  }

  ~PosixPipeOutputStream() override { Close(); }

  void WriteAsync(scoped_refptr<IOBuffer> buf,
                  std::size_t buf_len,
                  IOWriteCallback callback) override {
    if (closed_) {
      if (callback) callback(false, 0u);
      return;
    }

    PendingWrite pw;
    pw.buf = std::move(buf);
    pw.buf_len = buf_len;
    pw.offset = 0;
    pw.callback = std::move(callback);
    writes_.push_back(std::move(pw));

    DrainWrites();
  }

  void Close() override {
    if (closed_) return;
    closed_ = true;
    controller_.StopWatching();
    while (!writes_.empty()) {
      if (writes_.front().callback) writes_.front().callback(false, 0u);
      writes_.pop_front();
    }
    if (fd_ >= 0) {
      (void)close(fd_);
      fd_ = -1;
    }
  }

  void OnFileCanReadWithoutBlocking(NativeIOHandle /*handle*/) override {}

  void OnFileCanWriteWithoutBlocking(NativeIOHandle handle) override {
    if (closed_ || static_cast<int>(handle) != fd_) return;
    DrainWrites();
  }

 private:
  void EnsureWriteWatch() {
    if (closed_ || pump_ == nullptr || fd_ < 0) return;
    controller_.StopWatching();
    (void)controller_.StartWatching(
        pump_, fd_, MessagePumpForIO::FdWatchController::Mode::WRITE, this);
  }

  void DrainWrites() {
    if (closed_) return;

    while (!writes_.empty()) {
      PendingWrite& front = writes_.front();
      const std::size_t remaining = front.buf_len - front.offset;
      if (remaining == 0) {
        if (front.callback) front.callback(true, front.buf_len);
        writes_.pop_front();
        continue;
      }

      const char* ptr = front.buf->data() + front.offset;
      const ssize_t n = write(fd_, ptr, remaining);
      if (n > 0) {
        front.offset += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        EnsureWriteWatch();
        return;
      }
      // Error.
      if (front.callback) front.callback(false, 0u);
      writes_.pop_front();
      Close();
      return;
    }

    controller_.StopWatching();
  }

  MessagePumpForIO* pump_ = nullptr;
  int fd_ = -1;
  bool closed_ = false;
  std::deque<PendingWrite> writes_;
  MessagePumpForIO::FdWatchController controller_;
};

}  // namespace

std::unique_ptr<AsyncInputStream> CreatePipeInputStream(MessagePumpForIO* pump,
                                                        NativeIOHandle handle) {
  return std::make_unique<PosixPipeInputStream>(pump, static_cast<int>(handle));
}

std::unique_ptr<AsyncOutputStream> CreatePipeOutputStream(
    MessagePumpForIO* pump,
    NativeIOHandle handle) {
  return std::make_unique<PosixPipeOutputStream>(
      pump, static_cast<int>(handle));
}

}  // namespace nei

#endif  // !defined(_WIN32)