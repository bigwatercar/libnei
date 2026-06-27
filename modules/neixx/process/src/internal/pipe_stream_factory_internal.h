#pragma once

#ifndef NEIXX_IO_INTERNAL_PIPE_STREAM_FACTORY_INTERNAL_H_
#define NEIXX_IO_INTERNAL_PIPE_STREAM_FACTORY_INTERNAL_H_

#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/io/async_stream.h>
#include <neixx/task/message_loop/message_pump_io.h>

namespace nei {

NEI_API std::unique_ptr<AsyncInputStream> CreatePipeInputStream(
    MessagePumpForIO* pump,
    NativeIOHandle handle);

NEI_API std::unique_ptr<AsyncOutputStream> CreatePipeOutputStream(
    MessagePumpForIO* pump,
    NativeIOHandle handle);

}  // namespace nei

#endif  // NEIXX_IO_INTERNAL_PIPE_STREAM_FACTORY_INTERNAL_H_