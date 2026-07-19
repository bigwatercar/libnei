#pragma once

#ifndef NEIXX_NET_TCP_CLIENT_SOCKET_H_
#define NEIXX_NET_TCP_CLIENT_SOCKET_H_

#include <functional>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/io/async_stream.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/net/ip_end_point.h>

namespace nei {

class TaskRunner;

namespace net {

// =============================================================================
// TCPClientSocket  --  async TCP connect + read/write
// =============================================================================
//
// Implements both AsyncInputStream and AsyncOutputStream.  After a successful
// Connect(), ReadAsync / WriteAsync operate on the TCP stream.
//
// Windows: ConnectEx + WSARecv/WSASend via IOCP.
// POSIX: non-blocking connect + epoll for read/write readiness.
//
// All callbacks execute on the caller-supplied |io_runner| passed to
// Connect().  Once connected, ReadAsync/WriteAsync use the IO pump associated
// with the socket (typically a shared I/O thread).
//
// Usage:
//   auto client = std::make_unique<TCPClientSocket>();
//   client->Connect(IPEndPoint(addr, 8080),
//                   [](bool ok) {
//                     if (ok) client->ReadAsync(buf, 1024, read_cb);
//                   },
//                   io_runner);
//
class NEI_API TCPClientSocket : public AsyncInputStream,
                                 public AsyncOutputStream {
 public:
  using ConnectCallback = std::function<void(bool success)>;

  TCPClientSocket();
  ~TCPClientSocket() override;

  TCPClientSocket(const TCPClientSocket&) = delete;
  TCPClientSocket& operator=(const TCPClientSocket&) = delete;

  // Initiates an asynchronous connection to |addr|.
  // |callback| is invoked on |io_runner| with the connect result.
  // Returns false if the socket could not be created or the connect
  // request could not be submitted.
  bool Connect(const IPEndPoint& addr,
               ConnectCallback callback,
               scoped_refptr<TaskRunner> io_runner);

  // ---- AsyncInputStream ------------------------------------------------

  void ReadAsync(scoped_refptr<IOBuffer> buf,
                 std::size_t buf_len,
                 IOReadCallback callback) override;

  void Close() override;

  // ---- Graceful Shutdown -----------------------------------------------

  // Shuts down the write side (POSIX: SHUT_WR, Windows: SD_SEND), sending
  // FIN to the peer while keeping the read side open to drain the peer's
  // response / EOF.  After calling ShutdownWrite(), WriteAsync() must not
  // be called  --  the write stream is closed.
  //
  // Close() is still available as a hard-close that stops all I/O immediately.
  void ShutdownWrite();

  // ---- AsyncOutputStream -----------------------------------------------

  void WriteAsync(scoped_refptr<IOBuffer> buf,
                  std::size_t buf_len,
                  IOWriteCallback callback) override;

 public:
  // Forward declaration for PIMPL.  Defined in src/.
  class Impl;

  // Constructed from an already-accepted socket by TCPServerSocket.
  // Takes ownership of |impl| (the shell adds a reference).
  explicit TCPClientSocket(Impl* impl);

 private:
  Impl* impl_ = nullptr;  // Raw pointer  --  lifetime managed by RefCountedThreadSafe
};

}  // namespace net
}  // namespace nei

#endif  // NEIXX_NET_TCP_CLIENT_SOCKET_H_
