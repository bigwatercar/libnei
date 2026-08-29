#include <neixx/io/stream_writer.h>

#include <cstring>
#include <string_view>
#include <utility>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/io/io_buffer.h>
#include <neixx/task/thread_task_runner_handle.h>

namespace nei {

namespace {

void PostWriteResult(scoped_refptr<TaskRunner> runner,
                     StreamWriter::WriteCallback callback,
                     bool success,
                     std::size_t bytes_written) {
  if (runner) {
    runner->PostTask(FROM_HERE, [callback = std::move(callback), success, bytes_written]() mutable {
      if (callback) {
        callback(success, bytes_written);
      }
    });
    return;
  }
  if (callback) {
    callback(success, bytes_written);
  }
}

} // namespace

StreamWriter::StreamWriter(AsyncOutputStream *stream)
    : stream_(stream)
    , target_task_runner_(ThreadTaskRunnerHandle::Get()) {
  DCHECK(stream_ != nullptr);
}

StreamWriter::~StreamWriter() {
  weak_factory_.InvalidateWeakPtrs(FROM_HERE);
}

void StreamWriter::WriteString(std::string_view text, WriteCallback user_callback) {
  if (stream_ == nullptr) {
    PostWriteResult(target_task_runner_, std::move(user_callback), false, 0u);
    return;
  }

  if (text.empty()) {
    PostWriteResult(target_task_runner_, std::move(user_callback), true, 0u);
    return;
  }

  const std::size_t len = text.size();
  scoped_refptr<PooledIOBuffer> sized_buffer = IOBufferPool::GetInstance().AcquireBuffer(len);
  std::memcpy(sized_buffer->data(), text.data(), len);
  scoped_refptr<IOBuffer> base_buffer(sized_buffer.get());

  auto weak_this = weak_factory_.GetWeakPtr(FROM_HERE);
  scoped_refptr<TaskRunner> target_runner = target_task_runner_;

  // Self-contained request closure model:
  //   - sized_buffer owns the temporary write bytes for exactly one request.
  //   - the buffer and user callback are moved into the async completion path.
  //   - after physical write completes, a logic-sequence trampoline performs
  //     WeakPtr gate check before running user callback.
  //   - if StreamWriter has already been destroyed, trampoline exits quietly,
  //     and closure teardown releases the temporary IO buffer automatically.
  stream_->WriteAsync(
      std::move(base_buffer),
      len,
      [weak_this, target_runner, sized_buffer, user_callback = std::move(user_callback)](
          bool success, std::size_t bytes_written) mutable {
        auto deliver =
            [weak_this, success, bytes_written, user_callback = std::move(user_callback), sized_buffer]() mutable {
              if (!weak_this) {
                return;
              }
              if (user_callback) {
                user_callback(success, bytes_written);
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
