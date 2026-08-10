#pragma once

#ifndef NEIXX_IO_STREAM_WRITER_H_
#define NEIXX_IO_STREAM_WRITER_H_

#include <cstddef>
#include <functional>
#include <string_view>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/io/async_stream.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/task/task_runner.h>

namespace nei {

// StreamWriter wraps AsyncOutputStream and owns temporary write buffers inside
// each request closure so business code only passes plain text.
class NEI_API StreamWriter final {
public:
  using WriteCallback = std::function<void(bool success, std::size_t bytes_written)>;

  explicit StreamWriter(AsyncOutputStream *stream);
  ~StreamWriter();

  StreamWriter(const StreamWriter &) = delete;
  StreamWriter &operator=(const StreamWriter &) = delete;

  // Writes |text| as one async request. The internal IOBuffer lifetime is tied
  // to the request closure and released automatically after completion.
  void WriteString(std::string_view text, WriteCallback user_callback);

private:
  AsyncOutputStream *stream_ = nullptr; // Non-owning.
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  scoped_refptr<SequencedTaskRunner> target_task_runner_;
  WeakPtrFactory<StreamWriter> weak_factory_{this, FROM_HERE_MEMBER};
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

} // namespace nei

#endif // NEIXX_IO_STREAM_WRITER_H_
