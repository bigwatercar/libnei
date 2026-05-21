#pragma once

#ifndef NEIXX_IO_ASYNC_STREAM_H_
#define NEIXX_IO_ASYNC_STREAM_H_

#include <cstdint>
#include <functional>
#include <vector>

#include <nei/macros/nei_export.h>
#include <neixx/task/message_loop/message_pump_io.h>

namespace nei {

class NEI_API AsyncInputStream {
 public:
  using DataCallback = std::function<void(std::vector<std::uint8_t>&& data)>;

  virtual ~AsyncInputStream() = default;

  virtual void ReadAsync(DataCallback callback) = 0;
  virtual void Close() = 0;
};

class NEI_API AsyncOutputStream {
 public:
  using WriteCompleteCallback = std::function<void(bool success)>;

  virtual ~AsyncOutputStream() = default;

  virtual void WriteAsync(std::vector<std::uint8_t> data,
                          WriteCompleteCallback callback) = 0;
  virtual void Close() = 0;
};

}  // namespace nei

#endif  // NEIXX_IO_ASYNC_STREAM_H_
