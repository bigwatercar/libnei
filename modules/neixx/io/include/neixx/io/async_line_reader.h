#pragma once

#ifndef NEIXX_IO_ASYNC_LINE_READER_H_
#define NEIXX_IO_ASYNC_LINE_READER_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nei/macros/nei_export.h>
#include <neixx/io/async_stream.h>

namespace nei {

class NEI_API AsyncLineReader {
 public:
  explicit AsyncLineReader(AsyncInputStream* input_stream);
  ~AsyncLineReader();

  using LineCallback = std::function<void(std::string&& line)>;
  void StartReadingLines(LineCallback callback);

 private:
    struct State;

    static void OnRawDataReceived(const std::shared_ptr<State>& state,
                                  std::vector<std::uint8_t>&& data);

  AsyncInputStream* stream_ = nullptr;
    std::shared_ptr<State> state_;
};

}  // namespace nei

#endif  // NEIXX_IO_ASYNC_LINE_READER_H_
