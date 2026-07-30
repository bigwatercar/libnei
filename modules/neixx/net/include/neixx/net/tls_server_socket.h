#pragma once
#ifndef NEIXX_NET_TLS_SERVER_SOCKET_H_
#define NEIXX_NET_TLS_SERVER_SOCKET_H_

#include <functional>
#include <memory>
#include <nei/macros/nei_export.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/tcp_server_socket.h>
#include <neixx/task/task_runner.h>

namespace nei::net {
class SSLContext;
class TLSClientSocket;
struct KeepAliveConfig;

class NEI_API TLSServerSocket {
 public:
  using AcceptCallback = std::function<void(bool, std::unique_ptr<TLSClientSocket>)>;
  using RunnerSelector = TCPServerSocket::RunnerSelector;
  explicit TLSServerSocket(SSLContext* ctx);
  ~TLSServerSocket();
  TLSServerSocket(const TLSServerSocket&) = delete;
  TLSServerSocket& operator=(const TLSServerSocket&) = delete;
  bool Listen(const IPEndPoint& addr, int backlog, AcceptCallback callback,
              scoped_refptr<TaskRunner> runner, RunnerSelector selector = {});
  void Close();

  // Configures OS-level TCP keep-alive on all future accepted connections.
  // The configuration is applied to the underlying TCP socket before the
  // TLS handshake starts.  Already-accepted connections are not affected.
  void SetKeepAlive(const KeepAliveConfig& config);

 private:
  class Impl;
  Impl* impl_ = nullptr;
};

}  // namespace nei::net
#endif
