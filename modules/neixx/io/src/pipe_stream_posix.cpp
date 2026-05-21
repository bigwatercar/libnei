#if !defined(_WIN32)

#include <neixx/io/pipe_stream_factory.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <unistd.h>

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
  if (initialized) {
    return;
  }
  (void)signal(SIGPIPE, SIG_IGN);
  initialized = true;
}

class PosixPipeInputStream final : public AsyncInputStream,
                                   public MessagePumpForIO::Watcher {
 public:
  PosixPipeInputStream(MessagePumpForIO* pump, int fd)
      : pump_(pump), fd_(fd) {}

  ~PosixPipeInputStream() override { Close(); }

  void ReadAsync(DataCallback callback) override {
    callback_ = std::move(callback);
    if (closed_ || pump_ == nullptr || fd_ < 0) {
      return;
    }
    if (!controller_.is_watching()) {
      (void)controller_.StartWatching(
          pump_, fd_, MessagePumpForIO::FdWatchController::Mode::READ, this);
    }
  }

  void Close() override {
    if (closed_) {
      return;
    }
    closed_ = true;
    controller_.StopWatching();
    if (fd_ >= 0) {
      (void)close(fd_);
      fd_ = -1;
    }
    if (callback_) {
      callback_({});
      callback_ = nullptr;
    }
  }

  void OnFileCanReadWithoutBlocking(NativeIOHandle handle) override {
    if (closed_ || callback_ == nullptr || static_cast<int>(handle) != fd_) {
      return;
    }

    for (;;) {
      std::vector<std::uint8_t> buffer(4096);
      const ssize_t n = read(fd_, buffer.data(), buffer.size());
      if (n > 0) {
        buffer.resize(static_cast<std::size_t>(n));
        callback_(std::move(buffer));
        continue;
      }
      if (n == 0) {
        Close();
        return;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      Close();
      return;
    }
  }

  void OnFileCanWriteWithoutBlocking(NativeIOHandle /*handle*/) override {}

 private:
  MessagePumpForIO* pump_ = nullptr;
  int fd_ = -1;
  bool closed_ = false;
  DataCallback callback_;
  MessagePumpForIO::FdWatchController controller_;
};

class PosixPipeOutputStream final : public AsyncOutputStream,
                                    public MessagePumpForIO::Watcher {
 public:
  struct PendingWrite {
    std::vector<std::uint8_t> data;
    std::size_t offset = 0;
    WriteCompleteCallback callback;
  };

  PosixPipeOutputStream(MessagePumpForIO* pump, int fd)
      : pump_(pump), fd_(fd) {
    IgnoreSigPipeOnce();
  }

  ~PosixPipeOutputStream() override { Close(); }

  void WriteAsync(std::vector<std::uint8_t> data,
                  WriteCompleteCallback callback) override {
    if (closed_) {
      if (callback) {
        callback(false);
      }
      return;
    }

    PendingWrite pending;
    pending.data = std::move(data);
    pending.callback = std::move(callback);
    writes_.push_back(std::move(pending));

    DrainWrites();
  }

  void Close() override {
    if (closed_) {
      return;
    }
    closed_ = true;
    controller_.StopWatching();
    while (!writes_.empty()) {
      if (writes_.front().callback) {
        writes_.front().callback(false);
      }
      writes_.pop_front();
    }
    if (fd_ >= 0) {
      (void)close(fd_);
      fd_ = -1;
    }
  }

  void OnFileCanReadWithoutBlocking(NativeIOHandle /*handle*/) override {}

  void OnFileCanWriteWithoutBlocking(NativeIOHandle handle) override {
    if (closed_ || static_cast<int>(handle) != fd_) {
      return;
    }
    DrainWrites();
  }

 private:
  void EnsureWatchMode(MessagePumpForIO::FdWatchController::Mode mode) {
    if (closed_ || pump_ == nullptr || fd_ < 0) {
      return;
    }
    controller_.StopWatching();
    (void)controller_.StartWatching(pump_, fd_, mode, this);
  }

  void DrainWrites() {
    if (closed_) {
      return;
    }

    while (!writes_.empty()) {
      PendingWrite& pending = writes_.front();
      const std::uint8_t* ptr = pending.data.data() + pending.offset;
      const std::size_t remaining = pending.data.size() - pending.offset;
      if (remaining == 0) {
        if (pending.callback) {
          pending.callback(true);
        }
        writes_.pop_front();
        continue;
      }

      const ssize_t n = write(fd_, ptr, remaining);
      if (n > 0) {
        pending.offset += static_cast<std::size_t>(n);
        continue;
      }

      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        EnsureWatchMode(MessagePumpForIO::FdWatchController::Mode::WRITE);
        return;
      }

      if (pending.callback) {
        pending.callback(false);
      }
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
  return std::make_unique<PosixPipeOutputStream>(pump, static_cast<int>(handle));
}

}  // namespace nei

#endif  // !defined(_WIN32)
