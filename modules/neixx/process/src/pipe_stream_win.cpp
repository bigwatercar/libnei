#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "internal/pipe_stream_factory_internal.h"

#include <cstddef>
#include <cstring>
#include <deque>
#include <exception>
#include <utility>

#include <neixx/io/io_buffer.h>
#include <neixx/task/message_loop/message_pump_io.h>

namespace nei {
namespace {

constexpr DWORD kIoDrainTimeoutMs = 5000;

bool HandleIoDrainWaitFailure(const char* stream_name, DWORD wait_rv) {
#if !defined(NDEBUG)
  (void)stream_name;
  (void)wait_rv;
  std::terminate();
#else
  OutputDebugStringA("[neixx][io] ");
  OutputDebugStringA(stream_name);
  if (wait_rv == WAIT_TIMEOUT) {
    OutputDebugStringA(" stream drain wait timed out; skipping close.\n");
  } else {
    OutputDebugStringA(" stream drain wait failed; skipping close.\n");
  }
  return false;
#endif
}

// ---------------------------------------------------------------------------
// WinPipeInputStream
//
// Pull model: one ReadAsync() <-> one ReadFile() <-> one callback.
//
// The caller-supplied IOBuffer is used directly as the ReadFile target region.
// The scoped_refptr keeps the buffer alive throughout the asynchronous gap.
// The caller must not issue another ReadAsync() while one is in flight.
// ---------------------------------------------------------------------------
class WinPipeInputStream final : public AsyncInputStream,
                                 public MessagePumpForIO::Watcher {
 public:
  WinPipeInputStream(MessagePumpForIO* pump, HANDLE handle)
      : pump_(pump), handle_(handle) {
    // Manual-reset event injected into the OVERLAPPED so the kernel can
    // signal it on completion or cancellation - used by Close() for safe
    // synchronous drain without requiring the IOCP pump to process anything.
    read_event_ = CreateEventW(nullptr, /*bManualReset=*/TRUE,
                               /*bInitialState=*/FALSE, nullptr);
    // Prevent synchronous ReadFile completions from queuing a spurious IOCP
    // notification.  Without this flag, a ReadFile that returns immediately
    // (e.g. ERROR_MORE_DATA on a message-mode pipe) still posts a completion
    // packet to the IOCP port, which would later be picked up as if it were
    // the *next* async request's completion, corrupting the IO state machine.
    (void)SetFileCompletionNotificationModes(
        handle_, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);
  }

  ~WinPipeInputStream() override {
    Close();
    if (read_event_ != nullptr) {
      CloseHandle(read_event_);
      read_event_ = nullptr;
    }
  }

  void ReadAsync(scoped_refptr<IOBuffer> buf,
                 std::size_t buf_len,
                 IOReadCallback callback) override {
    if (closed_) {
      if (callback) callback(false, 0u);
      return;
    }
    if (read_in_flight_) {
      // Overlapping reads not supported.
      if (callback) callback(false, 0u);
      return;
    }

    // Store pending state before issuing the OS call.
    pending_buf_ = std::move(buf);
    pending_len_ = buf_len;
    pending_cb_ = std::move(callback);

    if (!controller_.is_watching()) {
      (void)controller_.StartWatching(
          pump_, reinterpret_cast<NativeIOHandle>(handle_),
          MessagePumpForIO::FdWatchController::Mode::READ, this);
    }

    IssueRead();
  }

  void Close() override {
    if (closed_) return;
    closed_ = true;
    controller_.StopWatching();

    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      if (read_in_flight_) {
        (void)CancelIoEx(handle_, &read_overlapped_);
        const DWORD wait_rv =
            WaitForSingleObject(read_event_, kIoDrainTimeoutMs);
        if (wait_rv != WAIT_OBJECT_0) {
          if (!HandleIoDrainWaitFailure("input", wait_rv)) return;
        }
        read_in_flight_ = false;
      }
      (void)CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }

    // Notify any pending ReadAsync caller that the stream is gone.
    if (pending_cb_) {
      IOReadCallback cb = std::move(pending_cb_);
      pending_buf_.reset();
      cb(false, 0u);
    }
  }

  void OnFileCanReadWithoutBlocking(NativeIOHandle handle) override {
    if (closed_ || reinterpret_cast<HANDLE>(handle) != handle_ ||
        !read_in_flight_) {
      return;
    }

    DWORD transferred = 0;
    const BOOL ok = GetOverlappedResult(handle_, &read_overlapped_,
                                        &transferred, FALSE);
    read_in_flight_ = false;

    if (!ok) {
      const DWORD err = GetLastError();
      if (err == ERROR_MORE_DATA && transferred > 0) {
        // Partial read (message-mode pipe): deliver what we have.
        DeliverSuccess(static_cast<std::size_t>(transferred));
        return;
      }
      // ERROR_BROKEN_PIPE, ERROR_HANDLE_EOF, or any other error -> EOF/error.
      DeliverEof();
      return;
    }

    if (transferred == 0) {
      DeliverEof();
      return;
    }

    DeliverSuccess(static_cast<std::size_t>(transferred));
  }

  void OnFileCanWriteWithoutBlocking(NativeIOHandle /*handle*/) override {}

 private:
  void IssueRead() {
    if (closed_ || !pending_buf_ || read_in_flight_) return;

    std::memset(&read_overlapped_, 0, sizeof(read_overlapped_));
    read_overlapped_.hEvent = read_event_;
    ResetEvent(read_event_);

    DWORD read_bytes = 0;
    const BOOL ok = ReadFile(handle_, pending_buf_->data(),
                             static_cast<DWORD>(pending_len_), &read_bytes,
                             &read_overlapped_);
    if (ok) {
      if (read_bytes == 0) {
        DeliverEof();
        return;
      }
      // Synchronous completion (rare for async handles, but handle it).
      DeliverSuccess(static_cast<std::size_t>(read_bytes));
      return;
    }

    const DWORD err = GetLastError();
    if (err == ERROR_MORE_DATA && read_bytes > 0) {
      DeliverSuccess(static_cast<std::size_t>(read_bytes));
      return;
    }
    if (err == ERROR_IO_PENDING) {
      read_in_flight_ = true;
      return;
    }
    if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) {
      DeliverEof();
      return;
    }
    DeliverEof();
  }

  void DeliverSuccess(std::size_t bytes) {
    IOReadCallback cb = std::move(pending_cb_);
    pending_buf_.reset();
    if (cb) cb(true, bytes);
    // NOTE: StopWatching is intentionally NOT called here. A HANDLE can only be
    // associated with an IOCP port once (CreateIoCompletionPort), so calling
    // StopWatching + StartWatching on the same handle would fail on the second
    // StartWatching.  We keep watching until Close().
  }

  void DeliverEof() {
    IOReadCallback cb = std::move(pending_cb_);
    pending_buf_.reset();
    if (cb) cb(false, 0u);
  }

  MessagePumpForIO* pump_ = nullptr;
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  HANDLE read_event_ = nullptr;
  bool closed_ = false;
  bool read_in_flight_ = false;

  // Pending read state: populated by ReadAsync(), consumed by delivery.
  scoped_refptr<IOBuffer> pending_buf_;
  std::size_t pending_len_ = 0;
  IOReadCallback pending_cb_;

  MessagePumpForIO::FdWatchController controller_;
  OVERLAPPED read_overlapped_{};
};

// ---------------------------------------------------------------------------
// WinPipeOutputStream
//
// Each WriteAsync() queues a PendingWrite that holds a scoped_refptr<IOBuffer>.
// The buffer region is passed directly to WriteFile(), so no extra copy is made.
// Multiple outstanding writes are serialised through the writes_ deque.
// The IOWriteCallback receives (success, bytes_written).
// ---------------------------------------------------------------------------
class WinPipeOutputStream final : public AsyncOutputStream,
                                  public MessagePumpForIO::Watcher {
 public:
  struct PendingWrite {
    scoped_refptr<IOBuffer> buf;
    std::size_t buf_len = 0;
    std::size_t offset = 0;   // Bytes already sent.
    IOWriteCallback callback;
  };

  WinPipeOutputStream(MessagePumpForIO* pump, HANDLE handle)
      : pump_(pump), handle_(handle) {
    write_event_ = CreateEventW(nullptr, /*bManualReset=*/TRUE,
                                /*bInitialState=*/FALSE, nullptr);
  }

  ~WinPipeOutputStream() override {
    Close();
    if (write_event_ != nullptr) {
      CloseHandle(write_event_);
      write_event_ = nullptr;
    }
  }

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

    if (!controller_.is_watching()) {
      (void)controller_.StartWatching(
          pump_, reinterpret_cast<NativeIOHandle>(handle_),
          MessagePumpForIO::FdWatchController::Mode::WRITE, this);
    }

    DrainWrites();
  }

  void Close() override {
    if (closed_) return;
    closed_ = true;
    controller_.StopWatching();

    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      if (write_in_flight_) {
        (void)CancelIoEx(handle_, &write_overlapped_);
        const DWORD wait_rv =
            WaitForSingleObject(write_event_, kIoDrainTimeoutMs);
        if (wait_rv != WAIT_OBJECT_0) {
          if (!HandleIoDrainWaitFailure("output", wait_rv)) return;
        }
        write_in_flight_ = false;
      }
      (void)CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }

    while (!writes_.empty()) {
      if (writes_.front().callback) writes_.front().callback(false, 0u);
      writes_.pop_front();
    }
  }

  void OnFileCanReadWithoutBlocking(NativeIOHandle /*handle*/) override {}

  void OnFileCanWriteWithoutBlocking(NativeIOHandle handle) override {
    if (closed_ || reinterpret_cast<HANDLE>(handle) != handle_) return;
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

    if (writes_.empty()) return;

    PendingWrite& front = writes_.front();
    front.offset += static_cast<std::size_t>(transferred);
    if (front.offset >= front.buf_len) {
      if (front.callback) front.callback(true, front.buf_len);
      writes_.pop_front();
    }

    DrainWrites();
  }

 private:
  void FailFrontAndClose() {
    if (!writes_.empty()) {
      if (writes_.front().callback) writes_.front().callback(false, 0u);
      writes_.pop_front();
    }
    Close();
  }

  void DrainWrites() {
    if (closed_ || write_in_flight_) return;

    while (!writes_.empty()) {
      PendingWrite& front = writes_.front();
      const std::size_t remaining = front.buf_len - front.offset;
      if (remaining == 0) {
        if (front.callback) front.callback(true, front.buf_len);
        writes_.pop_front();
        continue;
      }

      std::memset(&write_overlapped_, 0, sizeof(write_overlapped_));
      write_overlapped_.hEvent = write_event_;
      ResetEvent(write_event_);

      DWORD written = 0;
      const BOOL ok = WriteFile(
          handle_,
          static_cast<const char*>(front.buf->data()) + front.offset,
          static_cast<DWORD>(remaining), &written, &write_overlapped_);
      if (ok) {
        front.offset += static_cast<std::size_t>(written);
        if (front.offset >= front.buf_len) {
          if (front.callback) front.callback(true, front.buf_len);
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
  HANDLE write_event_ = nullptr;
  bool closed_ = false;
  bool write_in_flight_ = false;
  std::deque<PendingWrite> writes_;
  MessagePumpForIO::FdWatchController controller_;
  OVERLAPPED write_overlapped_{};
};

}  // namespace

std::unique_ptr<AsyncInputStream> CreatePipeInputStream(MessagePumpForIO* pump,
                                                        NativeIOHandle handle) {
  return std::make_unique<WinPipeInputStream>(
      pump, reinterpret_cast<HANDLE>(handle));
}

std::unique_ptr<AsyncOutputStream> CreatePipeOutputStream(
    MessagePumpForIO* pump,
    NativeIOHandle handle) {
  return std::make_unique<WinPipeOutputStream>(
      pump, reinterpret_cast<HANDLE>(handle));
}

}  // namespace nei

#endif  // defined(_WIN32)