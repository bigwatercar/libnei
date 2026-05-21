#pragma once

#ifndef NEIXX_IO_PIPE_STREAM_FACTORY_H_
#define NEIXX_IO_PIPE_STREAM_FACTORY_H_

#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/io/async_stream.h>

namespace nei {

class MessagePumpForIO;

NEI_API std::unique_ptr<AsyncInputStream> CreatePipeInputStream(
    MessagePumpForIO* pump,
    NativeIOHandle handle);

NEI_API std::unique_ptr<AsyncOutputStream> CreatePipeOutputStream(
    MessagePumpForIO* pump,
    NativeIOHandle handle);

}  // namespace nei

#endif  // NEIXX_IO_PIPE_STREAM_FACTORY_H_
