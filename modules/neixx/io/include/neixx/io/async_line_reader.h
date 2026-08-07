#pragma once

#ifndef NEIXX_IO_ASYNC_LINE_READER_H_
#define NEIXX_IO_ASYNC_LINE_READER_H_

#include <functional>
#include <memory>
#include <string>

#include <nei/build/nei_export.h>
#include <neixx/io/async_stream.h>
#include <nei/build/compiler_specific.h>

namespace nei {

// AsyncLineReader reads text lines from an AsyncInputStream.
//
// Pull model
// ----------
// Internally the reader issues ReadAsync() calls one at a time.  After each
// read completes it parses out complete lines and then issues another
// ReadAsync() to fetch the next chunk.  The caller simply supplies a
// LineCallback via StartReadingLines() and receives lines as they arrive.
//
// Thread safety
// -------------
// StartReadingLines() and FlushPendingLine() must be called from the same
// thread that drives stream_->ReadAsync().  Internally the reader is
// lock-free; all state is accessed only from that thread.
class NEI_API AsyncLineReader {
public:
  explicit AsyncLineReader(AsyncInputStream *input_stream);
  ~AsyncLineReader();

  using LineCallback = std::function<void(std::string &&line)>;

  // Begin reading lines from the stream.  callback is invoked once per
  // complete line (including CRLF handling).  May only be called once.
  void StartReadingLines(LineCallback callback);

  // Flush any bytes not yet terminated by a newline as a partial line.
  // Useful when callers know the stream has closed but want to recover
  // trailing output.  No-op if there is no pending data.
  void FlushPendingLine();

private:
  struct State;
  static void IssueNextRead(const std::shared_ptr<State> &state);
  static void OnChunkReceived(const std::shared_ptr<State> &state, bool ok, std::size_t bytes_read);

  AsyncInputStream *stream_ = nullptr;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::shared_ptr<State> state_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

} // namespace nei

#endif // NEIXX_IO_ASYNC_LINE_READER_H_