#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <neixx/io/pipe_stream_factory.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include <neixx/task/message_loop/message_pump_io.h>

namespace nei {
namespace {

class WinPipeInputStream final : public AsyncInputStream,
                                 public MessagePumpForIO::Watcher {
 public:
  WinPipeInputStream(MessagePumpForIO* pump, HANDLE handle)
      : pump_(pump), handle_(handle) {}

  ~WinPipeInputStream() override { Close(); }

  void ReadAsync(DataCallback callback) override {
    callback_ = std::move(callback);
    if (closed_ || pump_ == nullptr || handle_ == INVALID_HANDLE_VALUE ||
        handle_ == nullptr) {
      return;
    }

    if (!controller_.is_watching()) {
      (void)controller_.StartWatching(
          pump_, reinterpret_cast<NativeIOHandle>(handle_),
          MessagePumpForIO::FdWatchController::Mode::READ, this);
    }

    if (!read_in_flight_) {
      IssueRead();
    }
  }

  void Close() override {
    if (closed_) {
      return;
    }
    closed_ = true;
    controller_.StopWatching();
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      (void)CancelIoEx(handle_, nullptr);
      (void)CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
    if (callback_) {
      callback_({});
      callback_ = nullptr;
    }
  }

  void OnFileCanReadWithoutBlocking(NativeIOHandle handle) override {
    if (closed_ || reinterpret_cast<HANDLE>(handle) != handle_ ||
        !read_in_flight_) {
      return;
    }

    DWORD transferred = 0;
    const BOOL ok = GetOverlappedResult(handle_, &read_overlapped_, &transferred,
                                        FALSE);
    read_in_flight_ = false;
    if (!ok) {
      const DWORD err = GetLastError();
      if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) {
        Close();
        return;
      }
      Close();
      return;
    }

    if (transferred == 0) {
      Close();
      return;
    }

    std::vector<std::uint8_t> data(read_buffer_.begin(),
                                   read_buffer_.begin() + transferred);
    if (callback_) {
      callback_(std::move(data));
    }

    if (!closed_) {
      IssueRead();
    }
  }

  void OnFileCanWriteWithoutBlocking(NativeIOHandle /*handle*/) override {}

 private:
  void IssueRead() {
    if (closed_ || callback_ == nullptr || read_in_flight_) {
      return;
    }

    std::memset(&read_overlapped_, 0, sizeof(read_overlapped_));
    DWORD read_bytes = 0;
    const BOOL ok = ReadFile(handle_, read_buffer_.data(),
                             static_cast<DWORD>(read_buffer_.size()), &read_bytes,
                             &read_overlapped_);
    if (ok) {
      if (read_bytes == 0) {
        Close();
        return;
      }
      std::vector<std::uint8_t> data(read_buffer_.begin(),
                                     read_buffer_.begin() + read_bytes);
      callback_(std::move(data));
      IssueRead();
      return;
    }

    const DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) {
      read_in_flight_ = true;
      return;
    }
    if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) {
      Close();
      return;
    }
    Close();
  }

  MessagePumpForIO* pump_ = nullptr;
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  bool closed_ = false;
  bool read_in_flight_ = false;
  DataCallback callback_;
  MessagePumpForIO::FdWatchController controller_;
  OVERLAPPED read_overlapped_{};
  std::array<std::uint8_t, 4096> read_buffer_{};
};

class WinPipeOutputStream final : public AsyncOutputStream,
                                  public MessagePumpForIO::Watcher {
 public:
  struct PendingWrite {
    std::vector<std::uint8_t> data;
    std::size_t offset = 0;
    WriteCompleteCallback callback;
  };

  WinPipeOutputStream(MessagePumpForIO* pump, HANDLE handle)
      : pump_(pump), handle_(handle) {}

  ~WinPipeOutputStream() override { Close(); }

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

    if (!controller_.is_watching()) {
      (void)controller_.StartWatching(
          pump_, reinterpret_cast<NativeIOHandle>(handle_),
          MessagePumpForIO::FdWatchController::Mode::WRITE, this);
    }

    DrainWrites();
  }

  void Close() override {
    if (closed_) {
      return;
    }
    closed_ = true;
    controller_.StopWatching();
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      (void)CancelIoEx(handle_, nullptr);
      (void)CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
    while (!writes_.empty()) {
      if (writes_.front().callback) {
        writes_.front().callback(false);
      }
      writes_.pop_front();
    }
  }

  void OnFileCanReadWithoutBlocking(NativeIOHandle /*handle*/) override {}

  void OnFileCanWriteWithoutBlocking(NativeIOHandle handle) override {
    if (closed_ || reinterpret_cast<HANDLE>(handle) != handle_) {
      return;
    }
    if (!write_in_flight_) {
      DrainWrites();
      return;
    }

    DWORD transferred = 0;
    const BOOL ok = GetOverlappedResult(handle_, &write_overlapped_,
                                        &transferred, FALSE);
    write_in_flight_ = false;
    if (!ok) {
      FailFrontAndClose();
      return;
    }

    if (writes_.empty()) {
      return;
    }

    PendingWrite& front = writes_.front();
    front.offset += static_cast<std::size_t>(transferred);
    if (front.offset >= front.data.size()) {
      if (front.callback) {
        front.callback(true);
      }
      writes_.pop_front();
    }

    DrainWrites();
  }

 private:
  void FailFrontAndClose() {
    if (!writes_.empty() && writes_.front().callback) {
      writes_.front().callback(false);
      writes_.pop_front();
    }
    Close();
  }

  void DrainWrites() {
    if (closed_ || write_in_flight_) {
      return;
    }

    while (!writes_.empty()) {
      PendingWrite& front = writes_.front();
      const std::size_t remaining = front.data.size() - front.offset;
      if (remaining == 0) {
        if (front.callback) {
          front.callback(true);
        }
        writes_.pop_front();
        continue;
      }

      std::memset(&write_overlapped_, 0, sizeof(write_overlapped_));
      DWORD written = 0;
      const BOOL ok = WriteFile(
          handle_, front.data.data() + front.offset,
          static_cast<DWORD>(remaining), &written, &write_overlapped_);
      if (ok) {
        front.offset += static_cast<std::size_t>(written);
        if (front.offset >= front.data.size()) {
          if (front.callback) {
            front.callback(true);
          }
          writes_.pop_front();
        }
        continue;
      }

      const DWORD err = GetLastError();
      if (err == ERROR_IO_PENDING) {
        write_in_flight_ = true;
        return;
      }
      FailFrontAndClose();
      return;
    }

    controller_.StopWatching();
  }

  MessagePumpForIO* pump_ = nullptr;
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  bool closed_ = false;
  bool write_in_flight_ = false;
  std::deque<PendingWrite> writes_;
  MessagePumpForIO::FdWatchController controller_;
  OVERLAPPED write_overlapped_{};
};

}  // namespace

std::unique_ptr<AsyncInputStream> CreatePipeInputStream(MessagePumpForIO* pump,
                                                        NativeIOHandle handle) {
  return std::make_unique<WinPipeInputStream>(pump, reinterpret_cast<HANDLE>(handle));
}

std::unique_ptr<AsyncOutputStream> CreatePipeOutputStream(
    MessagePumpForIO* pump,
    NativeIOHandle handle) {
  return std::make_unique<WinPipeOutputStream>(pump, reinterpret_cast<HANDLE>(handle));
}

}  // namespace nei

#endif  // defined(_WIN32)
