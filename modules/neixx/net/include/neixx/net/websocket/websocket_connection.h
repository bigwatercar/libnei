#pragma once

#ifndef NEIXX_NET_WEBSOCKET_WEBSOCKET_CONNECTION_H_
#define NEIXX_NET_WEBSOCKET_WEBSOCKET_CONNECTION_H_

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/net/websocket/websocket_frame.h>

// =============================================================================
// WebSocketConnection — send handle for WebSocket frame handlers
// =============================================================================

namespace nei::net::websocket {

NEI_SUPPRESS_MSC_WARNING_4251_BEGIN

class NEI_API WebSocketConnection {
public:
  using SendRawCallback = std::function<void(std::vector<uint8_t>)>;
  using CloseCallback = std::function<void()>;

  WebSocketConnection(SendRawCallback send_raw, CloseCallback close_cb);

  void SendText(std::string_view text);
  void SendBinary(const void *data, size_t len);
  void SendClose(uint16_t code = 1000);
  void SendPing(const void *data = nullptr, size_t len = 0);
  void SendPong(const void *data = nullptr, size_t len = 0);
  void Close();

private:
  SendRawCallback send_raw_;
  CloseCallback close_;
};

NEI_SUPPRESS_MSC_WARNING_4251_END

} // namespace nei::net::websocket

#endif // NEIXX_NET_WEBSOCKET_WEBSOCKET_CONNECTION_H_
