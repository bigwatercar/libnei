#pragma once

#ifndef NEIXX_NET_TLS_CLIENT_SOCKET_H_
#define NEIXX_NET_TLS_CLIENT_SOCKET_H_

#include <cstddef>
#include <functional>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/io/async_stream.h>
#include <neixx/net/tcp_client_socket.h>
#include <neixx/task/task_runner.h>

namespace nei::net {

class SSLContext;

// =============================================================================
// TLSClientSocket — async TLS stream wrapping a TCPClientSocket
// =============================================================================
//
// TLSClientSocket adds TLS encryption/decryption on top of an existing
// TCPClientSocket.  It implements AsyncInputStream and AsyncOutputStream,
// so it is a drop-in replacement for TCPClientSocket in most code.
//
// Lifecycle:
//   1. Create with a TCPClientSocket and SSLContext (Client mode context).
//   2. Call Connect() → async TLS handshake over the TCP connection.
//   3. On handshake success → ReadAsync() / WriteAsync() encrypt/decrypt.
//   4. Close() sends TLS close_notify and closes the transport.
//
// Thread-safety: all public methods must be called on the IO thread
// associated with the provided TaskRunner, matching TCPClientSocket.
//
class NEI_API TLSClientSocket : public AsyncInputStream,
                                public AsyncOutputStream {
 public:
  using ConnectCallback = std::function<void(bool success)>;

  // Constructs a TLS client over an existing TCP socket.  The transport
  // must be freshly created (not yet connected).  Ownership is transferred.
  // `ctx` must outlive this socket.
  TLSClientSocket(std::unique_ptr<TCPClientSocket> transport,
                  SSLContext* ctx);
  ~TLSClientSocket() override;

  TLSClientSocket(const TLSClientSocket&) = delete;
  TLSClientSocket& operator=(const TLSClientSocket&) = delete;
  TLSClientSocket(TLSClientSocket&&) noexcept;
  TLSClientSocket& operator=(TLSClientSocket&&) noexcept;

  // -------------------------------------------------------------------------
  // Connect — start TCP connect + TLS handshake
  // -------------------------------------------------------------------------
  // Initiates the TCP connection and, on success, performs the TLS
  // handshake.  The callback fires once (with true/false) on completion.
  // Must be called on `runner`.
  void Connect(const IPEndPoint& addr,
               ConnectCallback callback,
               scoped_refptr<TaskRunner> runner);

  // Starts the TLS handshake on an already-connected transport.
  // Use this when the transport was obtained via accept() rather than
  // connect() (i.e., server-side TLS).  `runner` is the IO thread for
  // the handshake and subsequent I/O.
  void StartHandshake(ConnectCallback callback,
                      scoped_refptr<TaskRunner> runner);

  // -------------------------------------------------------------------------
  // AsyncInputStream / AsyncOutputStream
  // -------------------------------------------------------------------------
  void ReadAsync(scoped_refptr<IOBuffer> buf,
                 std::size_t buf_len,
                 IOReadCallback callback) override;
  void WriteAsync(scoped_refptr<IOBuffer> buf,
                  std::size_t buf_len,
                  IOWriteCallback callback) override;
  void Close() override;

  // Returns the ALPN protocol negotiated during the TLS handshake.
  // Empty string if ALPN was not configured or the handshake has not
  // completed.
  std::string GetNegotiatedProtocol() const;

  // ---- Keep-Alive (delegates to underlying TCP transport) ---------------

  // Enables or disables OS-level TCP keep-alive on the underlying socket.
  // Must be called after a successful handshake.
  bool SetKeepAlive(const KeepAliveConfig& config);

  // Starts a periodic health-check timer on the underlying TCP socket.
  // Delegates to TCPClientSocket::StartKeepAliveMonitor.
  void StartKeepAliveMonitor(TimeDelta check_interval,
                             OnceCallback<void()> on_dead);

  // Stops the keep-alive health monitor on the underlying TCP socket.
  void StopKeepAliveMonitor();

 private:
  class Impl;
  Impl* impl_ = nullptr;
};

}  // namespace nei::net

#endif  // NEIXX_NET_TLS_CLIENT_SOCKET_H_
