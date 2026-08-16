// WebSocket frame parser and builder — internal implementation details.
// NOT part of the public API.  Consumers use WebSocketConnection + WebSocketFrame.

#ifndef NEIXX_NET_WEBSOCKET_WEBSOCKET_FRAME_INTERNAL_H_
#define NEIXX_NET_WEBSOCKET_WEBSOCKET_FRAME_INTERNAL_H_

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <neixx/net/websocket/websocket_frame.h>

namespace nei::net::websocket {

// ---------------------------------------------------------------------------
// WebSocketFrameParser — incremental frame parser (internal)
// ---------------------------------------------------------------------------
class WebSocketFrameParser {
public:
  WebSocketFrameParser();
  ~WebSocketFrameParser();

  int64_t Parse(const uint8_t *data, size_t len);
  bool is_frame_complete() const;
  const WebSocketFrame &frame() const;
  void Reset();

private:
  enum class State {
    kHeader,
    kExtendedLen16,
    kExtendedLen64,
    kMaskingKey,
    kPayload,
    kComplete,
    kError,
  };

  State state_ = State::kHeader;
  WebSocketFrame frame_;
  size_t payload_read_ = 0;
};

// ---------------------------------------------------------------------------
// WebSocketFrameBuilder — serialize a frame to bytes (internal)
// ---------------------------------------------------------------------------
class WebSocketFrameBuilder {
public:
  static std::vector<uint8_t>
  Build(bool fin, WebSocketOpcode opcode, bool masked, const void *payload, size_t payload_len);

  static std::vector<uint8_t> BuildText(std::string_view text) {
    return Build(true, WebSocketOpcode::kText, false, text.data(), text.size());
  }

  static std::vector<uint8_t> BuildClose(uint16_t code = 1000, std::string_view reason = {});

  static std::vector<uint8_t> BuildPing(const void *payload = nullptr, size_t len = 0);

  static std::vector<uint8_t> BuildPong(const void *payload = nullptr, size_t len = 0);

  static void ApplyMask(const uint8_t masking_key[4], uint8_t *data, size_t len);
};

} // namespace nei::net::websocket

#endif // NEIXX_NET_WEBSOCKET_WEBSOCKET_FRAME_INTERNAL_H_
