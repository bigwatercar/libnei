#pragma once

#ifndef NEIXX_NET_TCP_CLIENT_SOCKET_H_
#define NEIXX_NET_TCP_CLIENT_SOCKET_H_

#include <functional>
#include <memory>

#include <nei/build/nei_export.h>
#include <neixx/common/time.h>
#include <neixx/functional/callback.h>
#include <neixx/io/async_stream.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/task/task_runner.h>

namespace nei {

namespace net {

class TLSServerSocket;

// =============================================================================
// KeepAliveConfig  --  TCP Keep-Alive socket options
// =============================================================================
//
// Enables OS-level TCP keep-alive probes (SO_KEEPALIVE) with configurable
// timing parameters.  When enabled, the kernel periodically sends probes
// on idle connections and marks the socket as errored if the peer does not
// respond.
//
// Platform mapping:
//   Windows: SIO_KEEPALIVE_VALS (millisecond granularity)
//   POSIX:   SO_KEEPALIVE + TCP_KEEPIDLE / TCP_KEEPINTVL / TCP_KEEPCNT
//
struct KeepAliveConfig {
  // Enable TCP keep-alive probes.  When false, the other fields are ignored.
  bool enable = false;

  // Time the connection must be idle before the first keep-alive probe.
  // Default: 30 seconds.
  TimeDelta idle_time = TimeDelta::FromSeconds(30);

  // Time between subsequent keep-alive probes when no ACK is received.
  // Default: 10 seconds.
  TimeDelta probe_interval = TimeDelta::FromSeconds(10);

  // Number of unacknowledged probes before the connection is declared dead.
  // With defaults: dead detected after ~60s (30 + 10*3).
  int probe_count = 3;
};

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
class NEI_API TCPClientSocket : public AsyncInputStream, public AsyncOutputStream {
public:
  using ConnectCallback = std::function<void(bool success)>;

  TCPClientSocket();
  ~TCPClientSocket() override;

  TCPClientSocket(const TCPClientSocket &) = delete;
  TCPClientSocket &operator=(const TCPClientSocket &) = delete;

  // Initiates an asynchronous connection to |addr|.
  // |callback| is invoked on |io_runner| with the connect result.
  // Returns false if the socket could not be created or the connect
  // request could not be submitted.
  bool Connect(const IPEndPoint &addr, ConnectCallback callback, scoped_refptr<SingleThreadTaskRunner> io_runner);

  // ---- AsyncInputStream ------------------------------------------------

  void ReadAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOReadCallback callback) override;

  void Close() override;

  // ---- Graceful Shutdown -----------------------------------------------

  // Shuts down the write side (POSIX: SHUT_WR, Windows: SD_SEND), sending
  // FIN to the peer while keeping the read side open to drain the peer's
  // response / EOF.  After calling ShutdownWrite(), WriteAsync() must not
  // be called  --  the write stream is closed.
  //
  // Close() is still available as a hard-close that stops all I/O immediately.
  void ShutdownWrite();

  // ---- Keep-Alive ------------------------------------------------------

  // Enables or disables OS-level TCP keep-alive probes on this socket.
  // Must be called after a successful Connect() (or after the socket is
  // accepted), on the socket's IO thread.
  //
  // Returns false if the socket is not connected or the platform
  // setsockopt / WSAIoctl call fails.
  bool SetKeepAlive(const KeepAliveConfig &config);

  // Starts a periodic health-check timer on the IO thread.  Every
  // |check_interval|, the socket's error state is polled via
  // getsockopt(SO_ERROR).  If the connection is detected as dead,
  // |on_dead| is invoked exactly once (on the IO thread) and the
  // timer is automatically stopped.
  //
  // Only one monitor can be active at a time.  Starting a new monitor
  // implicitly stops the previous one.  Must be called after a successful
  // Connect(), on the socket's IO thread.
  //
  // The monitor is complementary to SetKeepAlive(): SetKeepAlive arms
  // the kernel probes, and the monitor periodically checks whether the
  // probes have marked the socket as dead.
  void StartKeepAliveMonitor(TimeDelta check_interval, OnceCallback<void()> on_dead);

  // Stops the keep-alive health monitor.  Safe to call multiple times
  // and from any thread (posts a task to the IO thread if needed).
  void StopKeepAliveMonitor();

  // ---- AsyncOutputStream -----------------------------------------------

  void WriteAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOWriteCallback callback) override;

public:
  // Forward declaration for PIMPL.  Defined in src/.
  class Impl;

  // Constructed from an already-accepted socket by TCPServerSocket.
  // Takes ownership of |impl| (the shell adds a reference).
  explicit TCPClientSocket(Impl *impl);

private:
  friend class TLSServerSocket;

  // Returns the IO TaskRunner this socket is bound to.
  scoped_refptr<SingleThreadTaskRunner> io_task_runner() const;

  Impl *impl_ = nullptr; // Raw pointer  --  lifetime managed by RefCountedThreadSafe
};

} // namespace net
} // namespace nei

#endif // NEIXX_NET_TCP_CLIENT_SOCKET_H_
