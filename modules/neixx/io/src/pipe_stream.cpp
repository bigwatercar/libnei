#include <neixx/io/pipe_stream.h>

#include <cstddef>
#include <memory>
#include <utility>

#include <neixx/common/location.h>
#include <neixx/common/platform_handle.h>
#include <neixx/functional/bind.h>
#include <neixx/task/task_runner.h>

#if defined(_WIN32)
#include "pipe_stream_win.h"
#else
#include "pipe_stream_posix.h"
#endif

namespace nei {

// ===========================================================================
// PipeInputStream — public forwarding
// ===========================================================================

PipeInputStream::PipeInputStream(scoped_refptr<TaskRunner> io_task_runner)
    : impl_(std::make_unique<Impl>(std::move(io_task_runner))) {}

PipeInputStream::~PipeInputStream() {
  if (!impl_) return;
  scoped_refptr<TaskRunner> runner = impl_->io_task_runner();
  Impl* raw = impl_.release();
  if (runner) {
    const bool posted = runner->PostTask(
        FROM_HERE, BindOnce(&Impl::ShutdownAndSelfDestruct, raw));
    if (!posted) {
      raw->ShutdownAndSelfDestruct();
    }
  } else {
    raw->ShutdownAndSelfDestruct();
  }
}

bool PipeInputStream::BindPlatformHandle(PlatformHandle handle) {
  return impl_->BindPlatformHandle(std::move(handle));
}

void PipeInputStream::ReadAsync(scoped_refptr<IOBuffer> buf,
                                std::size_t buf_len,
                                IOReadCallback callback) {
  impl_->ReadAsync(std::move(buf), buf_len, std::move(callback));
}

void PipeInputStream::Close() { impl_->Close(); }

// ===========================================================================
// PipeOutputStream — public forwarding
// ===========================================================================

PipeOutputStream::PipeOutputStream(scoped_refptr<TaskRunner> io_task_runner)
    : impl_(std::make_unique<Impl>(std::move(io_task_runner))) {}

PipeOutputStream::~PipeOutputStream() {
  if (!impl_) return;
  scoped_refptr<TaskRunner> runner = impl_->io_task_runner();
  Impl* raw = impl_.release();
  if (runner) {
    const bool posted = runner->PostTask(
        FROM_HERE, BindOnce(&Impl::ShutdownAndSelfDestruct, raw));
    if (!posted) {
      raw->ShutdownAndSelfDestruct();
    }
  } else {
    raw->ShutdownAndSelfDestruct();
  }
}

bool PipeOutputStream::BindPlatformHandle(PlatformHandle handle) {
  return impl_->BindPlatformHandle(std::move(handle));
}

void PipeOutputStream::WriteAsync(scoped_refptr<IOBuffer> buf,
                                  std::size_t buf_len,
                                  IOWriteCallback callback) {
  impl_->WriteAsync(std::move(buf), buf_len, std::move(callback));
}

void PipeOutputStream::Close() { impl_->Close(); }

}  // namespace nei
