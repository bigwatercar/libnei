#ifndef NEIXX_IO_ASYNC_HANDLE_INTERNAL_H_
#define NEIXX_IO_ASYNC_HANDLE_INTERNAL_H_

#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>

#include <neixx/io/async_handle.h>
#include <neixx/io/io_context.h>

#if defined(_WIN32)
#include <Windows.h>
#include "io_context_internal.h"
#else
#include <sys/epoll.h>
#endif

namespace nei {

class AsyncHandle::Impl {
public:
  Impl(IOContext &context, FileHandle handle);
  ~Impl();

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  scoped_refptr<IOOperationToken> Read(const scoped_refptr<IOBuffer> &buffer,
                                       std::size_t len,
                                       IOResultCallback cb,
                                       const IOOperationOptions &options);
  scoped_refptr<IOOperationToken> Write(const scoped_refptr<IOBuffer> &buffer,
                                        std::size_t len,
                                        IOResultCallback cb,
                                        const IOOperationOptions &options);
  void Close();

private:
#if defined(_WIN32)
  struct WinOp final : public IOOverlappedBase {
    IOResultCallback cb;
    scoped_refptr<IOBuffer> buffer;
    scoped_refptr<IOOperationState> state;
    bool cancelled = false;
  };
#else
  struct PendingOp {
    scoped_refptr<IOBuffer> buffer;
    std::size_t len = 0;
    IOResultCallback cb;
    scoped_refptr<IOOperationState> state;
  };

  static constexpr uint32_t kReadyRead = EPOLLIN | EPOLLHUP | EPOLLERR;
  static constexpr uint32_t kReadyWrite = EPOLLOUT | EPOLLHUP | EPOLLERR;
  static constexpr std::size_t kMaxOpsPerWake = 16;
#endif

  bool IsHandleValidLocked() const;
  scoped_refptr<IOOperationToken> PrepareOperation(const IOOperationOptions &options,
                                                   IOResultCallback cb,
                                                   scoped_refptr<IOOperationState> *out_state);

#if defined(_WIN32)
  static void OnWindowsIoCompleted(IOOverlappedBase *base, DWORD bytes_transferred, DWORD error_code);
  bool BeginWindowsRead(const scoped_refptr<IOBuffer> &buffer,
                        std::size_t len,
                        IOResultCallback cb,
                        const scoped_refptr<IOOperationState> &state);
  bool BeginWindowsWrite(const scoped_refptr<IOBuffer> &buffer,
                         std::size_t len,
                         IOResultCallback cb,
                         const scoped_refptr<IOOperationState> &state);
#else
  bool EnqueueRead(PendingOp op);
  bool EnqueueWrite(PendingOp op);
  void OnLinuxEvents(uint32_t events);
  void DrainLinuxReadReady();
  void DrainLinuxWriteReady();
  void UpdateLinuxInterestLocked();
#endif

  IOContext *context_ = nullptr;
  FileHandle handle_;
  std::mutex mutex_;
  std::atomic<bool> closed_{false};

#if !defined(_WIN32)
  bool interest_dirty_ = false;
  bool prev_want_read_ = false;
  bool prev_want_write_ = false;

  std::deque<PendingOp> pending_reads_;
  std::deque<PendingOp> pending_writes_;
  std::shared_ptr<int> lifetime_token_;
#endif
};

} // namespace nei

#endif // NEIXX_IO_ASYNC_HANDLE_INTERNAL_H_
