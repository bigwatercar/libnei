#include <neixx/io/stream_reader.h>

#include <string>
#include <utility>
#include <vector>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/io/io_buffer.h>
#include <neixx/task/thread_task_runner_handle.h>

namespace nei {

namespace {

template <typename Callback>
void PostEmptyFailure(scoped_refptr<TaskRunner> runner, Callback callback) {
  if (runner) {
    runner->PostTask(FROM_HERE, [callback = std::move(callback)]() mutable {
      if (callback) {
        callback(false, {});
      }
    });
    return;
  }
  if (callback) {
    callback(false, {});
  }
}

} // namespace

StreamReader::StreamReader(AsyncInputStream *stream)
    : stream_(stream)
    , target_task_runner_(ThreadTaskRunnerHandle::Get()) {
  DCHECK(stream_ != nullptr);
}

StreamReader::~StreamReader() {
  weak_factory_.InvalidateWeakPtrs(FROM_HERE);
}

void StreamReader::ReadBytes(std::size_t bytes_to_read, ReadBytesCallback user_callback) {
  if (stream_ == nullptr || bytes_to_read == 0) {
    PostEmptyFailure(target_task_runner_, std::move(user_callback));
    return;
  }

  scoped_refptr<PooledIOBuffer> sized_buffer = IOBufferPool::GetInstance().AcquireBuffer(bytes_to_read);
  scoped_refptr<IOBuffer> base_buffer(sized_buffer.get());

  auto weak_this = weak_factory_.GetWeakPtr(FROM_HERE);
  scoped_refptr<TaskRunner> target_runner = target_task_runner_;

  // Thread trampoline contract:
  //   1) Request is issued to the physical stream with sized_buffer captured by
  //      scoped_refptr, so DMA destination stays valid across IO latency.
  //   2) Physical IO completion may happen on any backend thread.
  //   3) Completion is posted back to target_runner (business logic sequence).
  //   4) On target_runner, WeakPtr gate drops stale callbacks after owner
  //      destruction, preventing shutdown-time UAF crashes.
  stream_->ReadAsync(
      std::move(base_buffer),
      bytes_to_read,
      [weak_this, target_runner, sized_buffer, user_callback = std::move(user_callback)](
          bool success, std::size_t bytes_read) mutable {
        auto deliver =
            [weak_this, sized_buffer, success, bytes_read, user_callback = std::move(user_callback)]() mutable {
              if (!weak_this) {
                return;
              }

              std::vector<std::uint8_t> output;
              if (success && bytes_read > 0) {
                // NOTE: do NOT bound by sized_buffer->size() here  --  the
                // pooled buffer's size() is bucket-normalized (>= bytes_to_read)
                // and is NOT a semantic data length.  bytes_read is the exact
                // amount the stream produced.
                const std::uint8_t *begin = reinterpret_cast<const std::uint8_t *>(sized_buffer->data());
                output.assign(begin, begin + bytes_read);
              } else {
                success = false;
              }

              if (user_callback) {
                user_callback(success, std::move(output));
              }
            };

        if (target_runner) {
          (void)target_runner->PostTask(FROM_HERE, std::move(deliver));
        } else {
          deliver();
        }
      });
}

void StreamReader::ReadString(std::size_t bytes_to_read, ReadStringCallback user_callback) {
  if (stream_ == nullptr || bytes_to_read == 0) {
    PostEmptyFailure(target_task_runner_, std::move(user_callback));
    return;
  }

  scoped_refptr<PooledIOBuffer> sized_buffer = IOBufferPool::GetInstance().AcquireBuffer(bytes_to_read);
  scoped_refptr<IOBuffer> base_buffer(sized_buffer.get());

  auto weak_this = weak_factory_.GetWeakPtr(FROM_HERE);
  scoped_refptr<TaskRunner> target_runner = target_task_runner_;

  // Cross-thread interaction point (physical thread -> logic sequence):
  //   The physical stream callback intentionally does not touch StreamReader
  //   state directly. It only snapshots completion data and posts a closure
  //   back to target_runner. That closure is the single serialization point
  //   where WeakPtr validity is checked and user callback is fired.
  stream_->ReadAsync(
      std::move(base_buffer),
      bytes_to_read,
      [weak_this, target_runner, sized_buffer, user_callback = std::move(user_callback)](
          bool success, std::size_t bytes_read) mutable {
        auto deliver =
            [weak_this, sized_buffer, success, bytes_read, user_callback = std::move(user_callback)]() mutable {
              if (!weak_this) {
                return;
              }

              std::string output;
              if (success && bytes_read > 0) {
                // See ReadBytes: bytes_read is the semantic length; the pooled
                // buffer's size() is bucket-normalized and must not bound it.
                output.assign(reinterpret_cast<const char *>(sized_buffer->data()), bytes_read);
              } else {
                success = false;
              }

              if (user_callback) {
                user_callback(success, std::move(output));
              }
            };

        if (target_runner) {
          (void)target_runner->PostTask(FROM_HERE, std::move(deliver));
        } else {
          deliver();
        }
      });
}

} // namespace nei
