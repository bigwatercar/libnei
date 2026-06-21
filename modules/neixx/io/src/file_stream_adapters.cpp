#include <neixx/io/file_stream_adapters.h>

#include <utility>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/io/async_file.h>
#include <neixx/io/io_buffer.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/thread_task_runner_handle.h>

// FileInputStreamAdapter captures WeakPtr in read-ahead lambdas that hop
// between IO thread and target runner.  The adapter uses atomics for its
// state, so WeakPtr dereference from any thread is safe.
namespace nei {
template <>
struct WeakPtrThreadSafe<FileInputStreamAdapter> : std::true_type {};
}  // namespace nei

namespace nei {

// ---------------------------------------------------------------------------
// FileInputStreamAdapter implementation
// ---------------------------------------------------------------------------

FileInputStreamAdapter::FileInputStreamAdapter(AsyncFile* file,
                                               std::uint64_t start_offset)
    : FileInputStreamAdapter(file, ThreadTaskRunnerHandle::Get(), start_offset) {
}

FileInputStreamAdapter::FileInputStreamAdapter(
    AsyncFile* file,
    scoped_refptr<TaskRunner> target_task_runner,
    std::uint64_t start_offset)
    : file_(file),
      position_(start_offset),
      target_task_runner_(std::move(target_task_runner)) {
  DCHECK(file_ != nullptr);
  DCHECK(target_task_runner_ != nullptr);
}

FileInputStreamAdapter::~FileInputStreamAdapter() {
  // Invalidate all pending WeakPtr-gated callbacks to prevent post-destruction
  // UAF crashes when backend I/O operations complete after this adapter is
  // destroyed.
  weak_factory_.InvalidateWeakPtrs();
}

void FileInputStreamAdapter::ReadAsync(scoped_refptr<IOBuffer> buf,
                                       std::size_t buf_len,
                                       IOReadCallback callback) {
  auto weak_this = weak_factory_.GetWeakPtr();
  if (!weak_this || !target_task_runner_) {
    return;
  }

  // Any-thread entrypoint: always marshal to target_task_runner_ first.
  target_task_runner_->PostTask(
      FROM_HERE,
      [weak_this, buf = std::move(buf), buf_len, callback = std::move(callback)]() mutable {
        if (!weak_this) {
          return;
        }
        weak_this->ReadAsyncOnTarget(std::move(buf), buf_len, std::move(callback));
      });
}

void FileInputStreamAdapter::ReadAsyncOnTarget(scoped_refptr<IOBuffer> buf,
                                               std::size_t buf_len,
                                               IOReadCallback callback) {
  if (closed_ || !file_ || !buf || buf_len == 0) {
    if (callback) {
      callback(false, 0);
    }
    return;
  }

  // Capture and consume logical read position only on target sequence.
  const std::uint64_t read_offset = position_;

  auto weak_this = weak_factory_.GetWeakPtr();
  scoped_refptr<TaskRunner> target_runner = target_task_runner_;

  file_->ReadAsync(
      std::move(buf), buf_len, read_offset,
      [weak_this, target_runner, user_callback = std::move(callback)](
        bool success, std::size_t bytes_read, AsyncFile::Error error) mutable {
        // This lambda runs on the backend thread (platform-specific I/O thread
        // pool). Post the result back to the target sequence for safe state
        // mutation and user callback delivery.
        (void)error;  // error not used; propagate success flag instead.
        auto deliver = [weak_this, success, bytes_read,
                        user_callback = std::move(user_callback)]() mutable {
          if (!weak_this) {
            return;
          }

          if (weak_this->closed_) {
            if (user_callback) {
              user_callback(false, 0);
            }
            return;
          }

          bool result_success = success;
          std::size_t result_bytes = bytes_read;

          // Advance read cursor only on successful completion. This executes on
          // target sequence, so position_ is sequence-confined and race-free.
          if (result_success && result_bytes > 0) {
            weak_this->position_ += result_bytes;
          } else {
            result_success = false;
            result_bytes = 0;
          }

          if (user_callback) {
            user_callback(result_success, result_bytes);
          }
        };

        if (target_runner) {
          target_runner->PostTask(FROM_HERE, std::move(deliver));
        } else {
          deliver();
        }
      });
}

void FileInputStreamAdapter::Close() {
  auto weak_this = weak_factory_.GetWeakPtr();
  if (!weak_this || !target_task_runner_) {
    return;
  }
  target_task_runner_->PostTask(FROM_HERE, [weak_this]() {
    if (!weak_this) {
      return;
    }
    weak_this->CloseOnTarget();
  });
}

void FileInputStreamAdapter::CloseOnTarget() {
  if (closed_) {
    return;
  }
  closed_ = true;
  if (file_) {
    file_->CloseAsync(nullptr);
  }
}

// ---------------------------------------------------------------------------
// FileOutputStreamAdapter implementation
// ---------------------------------------------------------------------------

FileOutputStreamAdapter::FileOutputStreamAdapter(AsyncFile* file,
                                                 std::uint64_t start_offset)
    : FileOutputStreamAdapter(file, ThreadTaskRunnerHandle::Get(), start_offset) {
}

FileOutputStreamAdapter::FileOutputStreamAdapter(
    AsyncFile* file,
    scoped_refptr<TaskRunner> target_task_runner,
    std::uint64_t start_offset)
    : file_(file),
      position_(start_offset),
      target_task_runner_(std::move(target_task_runner)) {
  DCHECK(file_ != nullptr);
  DCHECK(target_task_runner_ != nullptr);
}

FileOutputStreamAdapter::~FileOutputStreamAdapter() {
  // Invalidate all pending WeakPtr-gated callbacks to prevent post-destruction
  // UAF crashes when backend I/O operations complete after this adapter is
  // destroyed.
  weak_factory_.InvalidateWeakPtrs();
}

void FileOutputStreamAdapter::WriteAsync(scoped_refptr<IOBuffer> buf,
                                         std::size_t bytes_to_write,
                                         IOWriteCallback callback) {
  auto weak_this = weak_factory_.GetWeakPtr();
  if (!weak_this || !target_task_runner_) {
    return;
  }

  // Any-thread entrypoint: always marshal to target_task_runner_ first.
  target_task_runner_->PostTask(
      FROM_HERE,
      [weak_this,
       buf = std::move(buf),
       bytes_to_write,
       callback = std::move(callback)]() mutable {
        if (!weak_this) {
          return;
        }
        weak_this->WriteAsyncOnTarget(std::move(buf), bytes_to_write, std::move(callback));
      });
}

void FileOutputStreamAdapter::WriteAsyncOnTarget(scoped_refptr<IOBuffer> buf,
                                                 std::size_t bytes_to_write,
                                                 IOWriteCallback callback) {
  if (closed_ || !file_ || !buf || bytes_to_write == 0) {
    if (callback) {
      callback(false, 0);
    }
    return;
  }

  // **Core anti-race mechanism**:
  // Atomically reserve an offset anchor on the logical thread.
  // This prevents concurrent WriteAsync() calls from trampling on each other's
  // offsets.
  //
  // Algorithm:
  //   1. Read current position_
  //   2. Atomically increment position_ by bytes_to_write
  //   3. Use the old value as the unique anchor for this write
  //
  // This is equivalent to:
  //   my_offset = position_;
  //   position_ += bytes_to_write;  (atomic)
  //
  // Key guarantee: reserve and advance write cursor immediately on target
  // logical sequence before dispatching physical I/O. This locks a unique
  // offset anchor for this request and prevents offset trampling.
  const std::uint64_t write_offset = position_;
  position_ += static_cast<std::uint64_t>(bytes_to_write);

  auto weak_this = weak_factory_.GetWeakPtr();
  scoped_refptr<TaskRunner> target_runner = target_task_runner_;

  // Dispatch the physical I/O to the backend at the pre-reserved offset.
  file_->WriteAsync(
      std::move(buf), bytes_to_write, write_offset,
      [weak_this, target_runner, user_callback = std::move(callback)](
        bool success, std::size_t bytes_written, AsyncFile::Error error) mutable {
        // This lambda runs on the backend thread (platform-specific I/O thread
        // pool). Post the result back to the target sequence for user callback
        // delivery.
        (void)error;  // error not used; propagate success flag instead.
        auto deliver = [weak_this, success, bytes_written,
                        user_callback = std::move(user_callback)]() mutable {
          if (!weak_this) {
            return;
          }

          if (weak_this->closed_) {
            if (user_callback) {
              user_callback(false, 0);
            }
            return;
          }

          if (user_callback) {
            user_callback(success, bytes_written);
          }
        };

        if (target_runner) {
          target_runner->PostTask(FROM_HERE, std::move(deliver));
        } else {
          deliver();
        }
      });
}

void FileOutputStreamAdapter::Close() {
  auto weak_this = weak_factory_.GetWeakPtr();
  if (!weak_this || !target_task_runner_) {
    return;
  }
  target_task_runner_->PostTask(FROM_HERE, [weak_this]() {
    if (!weak_this) {
      return;
    }
    weak_this->CloseOnTarget();
  });
}

void FileOutputStreamAdapter::CloseOnTarget() {
  if (closed_) {
    return;
  }
  closed_ = true;
  if (file_) {
    file_->CloseAsync(nullptr);
  }
}

}  // namespace nei
