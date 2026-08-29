// WebSocketConnection implementation.

#include <neixx/net/websocket/websocket_connection.h>

#include "websocket_frame_internal.h"

namespace nei::net::websocket {

WebSocketConnection::WebSocketConnection(
    WebSocketConnection::SendRawCallback send_raw,
    WebSocketConnection::CloseCallback close_cb)
    : send_raw_(std::move(send_raw))
    , close_(std::move(close_cb)) {
}

void WebSocketConnection::SendText(std::string_view text) {
  send_raw_(WebSocketFrameBuilder::BuildText(text));
}

void WebSocketConnection::SendBinary(const void *data, size_t len) {
  send_raw_(WebSocketFrameBuilder::Build(true, WebSocketOpcode::kBinary, false, data, len));
}

void WebSocketConnection::SendClose(uint16_t code) {
  send_raw_(WebSocketFrameBuilder::BuildClose(code));
}

void WebSocketConnection::SendPing(const void *data, size_t len) {
  send_raw_(WebSocketFrameBuilder::BuildPing(data, len));
}

void WebSocketConnection::SendPong(const void *data, size_t len) {
  send_raw_(WebSocketFrameBuilder::BuildPong(data, len));
}

void WebSocketConnection::Close() {
  if (close_)
    close_();
}

} // namespace nei::net::websocket
