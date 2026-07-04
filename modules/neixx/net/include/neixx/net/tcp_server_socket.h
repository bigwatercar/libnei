#pragma once

#ifndef NEIXX_NET_TCP_SERVER_SOCKET_H_
#define NEIXX_NET_TCP_SERVER_SOCKET_H_

#include <functional>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/net/ip_end_point.h>

namespace nei {

class TaskRunner;

namespace net {

class TCPClientSocket;

// =============================================================================
// TCPServerSocket — async TCP listen + accept
// =============================================================================
//
// Windows: AcceptEx + IOCP.  POSIX: accept4 + epoll.
//
// The server pre-allocates accept buffers and posts them to the kernel BEFORE
// the client arrives.  When a connection is accepted, the callback receives a
// fully-initialized TCPClientSocket that is immediately usable for async I/O.
//
// Multi-Reactor support (Acceptor-Worker model):
//   |worker_selector| is an optional factory that returns the IO runner for
//   each accepted connection.  When not provided, the server's own IO runner
//   is used (all clients share the acceptor thread).  To spread load across
//   multiple IO threads, provide a round-robin or least-connection selector:
//
//     server->Listen(addr, backlog, accept_cb,
//                    acceptor_io_runner,
//                    [&, next=0]() mutable {
//                      return workers[next++ % N]->GetTaskRunner();
//                    });
//
// All callbacks execute on the caller-supplied |acceptor_runner|.  No callback
// is ever invoked synchronously from Listen().
//
class NEI_API TCPServerSocket final {
 public:
  // |success| is true when a new connection was accepted.  |client| is
  // a fully-connected, non-blocking TCPClientSocket ready for async I/O.
  // On failure or shutdown, |success| is false and |client| is null.
  using AcceptCallback =
      std::function<void(bool success, std::unique_ptr<TCPClientSocket> client)>;

  // Called for each accepted connection to select which IO thread the
  // client socket will run on.  Return nullptr to use the server's own
  // IO runner (backward-compatible default).
  using RunnerSelector = std::function<scoped_refptr<TaskRunner>()>;

  TCPServerSocket();
  ~TCPServerSocket();

  TCPServerSocket(const TCPServerSocket&) = delete;
  TCPServerSocket& operator=(const TCPServerSocket&) = delete;

  // Starts listening on |addr|:|port| with the given |backlog|.
  // |callback| is invoked on |acceptor_runner| for each accepted connection.
  // |worker_selector| optionally assigns each client to a different IO thread.
  // Returns false if the socket could not be created or bound.
  bool Listen(const IPEndPoint& addr,
              int backlog,
              AcceptCallback callback,
              scoped_refptr<TaskRunner> acceptor_runner,
              RunnerSelector worker_selector = {});

  // Stops listening and cancels all pending accept operations.
  // Fires |callback| with |success=false| to notify the caller.
  void Close();

  // Silently stops listening without firing the accept callback.
  // Use for graceful shutdown when the caller already knows the server
  // is stopping.  In-flight accept4/AcceptEx operations complete quietly.
  void Shutdown();

 public:
  // Forward declaration for PIMPL.  Defined in src/.
  class Impl;

 private:
  void Orphan();

  Impl* impl_ = nullptr;  // Raw pointer — lifetime managed by RefCountedThreadSafe
};

}  // namespace net
}  // namespace nei

#endif  // NEIXX_NET_TCP_SERVER_SOCKET_H_
