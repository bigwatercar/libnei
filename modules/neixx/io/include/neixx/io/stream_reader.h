#pragma once

#ifndef NEIXX_IO_STREAM_READER_H_
#define NEIXX_IO_STREAM_READER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nei/macros/nei_export.h>
#include <neixx/io/async_stream.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/task/task_runner.h>
#include <nei/macros/suppress_compiler_warnings.h>

namespace nei {

// StreamReader wraps AsyncInputStream and provides payload-oriented callbacks.
//
// Design goals:
//   1) Business code does not need to allocate or parse IOBuffer directly.
//   2) Each read request captures buffer + callback in one closure context.
//   3) Completion is always bounced to the construction sequence.
//   4) WeakPtr gate drops late completions after destruction safely.
class NEI_API StreamReader final {
public:
  using ReadBytesCallback = std::function<void(bool success, std::vector<std::uint8_t> &&data)>;
  using ReadStringCallback = std::function<void(bool success, std::string &&data)>;

  explicit StreamReader(AsyncInputStream *stream);
  ~StreamReader();

  StreamReader(const StreamReader &) = delete;
  StreamReader &operator=(const StreamReader &) = delete;

  // Reads up to |bytes_to_read| bytes and returns an owning byte vector.
  void ReadBytes(std::size_t bytes_to_read, ReadBytesCallback user_callback);

  // Reads up to |bytes_to_read| bytes and returns an owning std::string.
  void ReadString(std::size_t bytes_to_read, ReadStringCallback user_callback);

private:
  AsyncInputStream *stream_ = nullptr; // Non-owning.
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  scoped_refptr<TaskRunner> target_task_runner_;
  WeakPtrFactory<StreamReader> weak_factory_{this, FROM_HERE_MEMBER};
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

} // namespace nei

#endif // NEIXX_IO_STREAM_READER_H_
