#pragma once

#ifndef NEIXX_IPC_RPC_ENDPOINT_H_
#define NEIXX_IPC_RPC_ENDPOINT_H_

#include <cstddef>
#include <functional>
#include <memory>

#include <nei/macros/nei_export.h>
#include <nei/macros/suppress_compiler_warnings.h>
#include <neixx/common/time.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/task/task_runner.h>

namespace nei {

class AsyncInputStream;
class AsyncOutputStream;

// ---------------------------------------------------------------------------
// RpcEndpoint  --  asynchronous RPC proxy engine over MessageChannel
// ---------------------------------------------------------------------------
//
// RpcEndpoint wraps MessageChannel and adds structured RPC semantics on top
// of the length-prefixed message framing.  The payload carried inside each
// MessageChannel frame is further structured as:
//
//   [1-byte MessageType][8-byte RequestID (LE)][Business Payload]
//
// Message types:
//   kOneWay   (0)  --  fire-and-forget, no response expected
//   kRequest  (1)  --  expects a kResponse with matching RequestID
//   kResponse (2)  --  carries the reply for a previous kRequest
//
// Thread safety
// -------------
// SendOneWay() and SendRequest() are safe to call from any thread.
// SetRequestHandler() must be called before Start().
// All user callbacks fire on |client_task_runner|.
//
// Timeout
// -------
// Each SendRequest() is guarded by a OneShotTimer.  If no kResponse
// arrives within |timeout|, the callback is invoked with a null
// IOBufferWithSize to signal timeout.
// ---------------------------------------------------------------------------
class NEI_API RpcEndpoint final {
public:
  // A payload buffer allocated from IOBufferPool.
  using MessageBuffer = scoped_refptr<IOBufferWithSize>;

  // Called by the request handler to send a response back to the caller.
  // The response payload is wrapped in a kResponse frame automatically.
  using ReplyCallback = std::function<void(MessageBuffer response)>;

  // Called when a response arrives (or times out with a null buffer).
  using ResponseCallback = std::function<void(MessageBuffer response)>;

  // Handler for incoming kRequest messages.
  // |request_payload|  --  the business payload after the RPC header.
  // |reply_cb|         --  call this to send a response back.
  using RequestHandler = std::function<void(MessageBuffer request_payload, ReplyCallback reply_cb)>;

  // Called on unrecoverable channel error.
  using ErrorHandler = std::function<void()>;

  // Constructs an RpcEndpoint.  Internally creates a MessageChannel on
  // the given streams with the two injected TaskRunners.
  //
  // |io_task_runner|      --  where I/O state-machine work happens.
  // |client_task_runner|  --  where all user callbacks are delivered.
  // |read_stream|         --  underlying async input stream (not owned).
  // |write_stream|        --  underlying async output stream (not owned).
  RpcEndpoint(scoped_refptr<SingleThreadTaskRunner> io_task_runner,
              scoped_refptr<SequencedTaskRunner> client_task_runner,
              AsyncInputStream *read_stream,
              AsyncOutputStream *write_stream);
  ~RpcEndpoint();

  RpcEndpoint(const RpcEndpoint &) = delete;
  RpcEndpoint &operator=(const RpcEndpoint &) = delete;

  // Starts listening for incoming messages.  Must be called after
  // SetRequestHandler().  |on_error| fires once on channel failure.
  void Start(ErrorHandler on_error);

  // Sends a one-way message.  No response is expected.
  void SendOneWay(MessageBuffer payload);

  // Sends a request and waits for a response.  |on_response| is called
  // with the response payload, or with a null buffer on timeout.
  void SendRequest(MessageBuffer payload, TimeDelta timeout, ResponseCallback on_response);

  // Registers the handler for incoming kRequest messages.
  // Must be called before Start().
  void SetRequestHandler(RequestHandler handler);

private:
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

} // namespace nei

#endif // NEIXX_IPC_RPC_ENDPOINT_H_
