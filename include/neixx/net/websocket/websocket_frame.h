#pragma once

#ifndef NEIXX_NET_WEBSOCKET_WEBSOCKET_FRAME_H_
#define NEIXX_NET_WEBSOCKET_WEBSOCKET_FRAME_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>

// =============================================================================
// WebSocket data framing (RFC 6455 §5)
// =============================================================================

namespace nei::net::websocket {

// ---------------------------------------------------------------------------
// Opcode — 4-bit frame type
// ---------------------------------------------------------------------------
enum class WebSocketOpcode : uint8_t {
  kContinuation = 0x0,
  kText = 0x1,
  kBinary = 0x2,
  kClose = 0x8,
  kPing = 0x9,
  kPong = 0xA,
};

// ---------------------------------------------------------------------------
// WebSocketFrame — a fully parsed frame
// ---------------------------------------------------------------------------
NEI_SUPPRESS_MSC_WARNING_4251_BEGIN

struct NEI_API WebSocketFrame {
  bool fin = true;
  WebSocketOpcode opcode = WebSocketOpcode::kText;
  bool masked = false;
  std::vector<uint8_t> masking_key;  // 4 bytes when masked=true
  std::vector<uint8_t> payload;

  // Convenience: get payload as string_view (for text frames).
  std::string_view text_payload() const {
    return std::string_view(reinterpret_cast<const char *>(payload.data()), payload.size());
  }

  // Convenience: check if this is a control frame.
  bool is_control() const {
    return opcode == WebSocketOpcode::kClose || opcode == WebSocketOpcode::kPing || opcode == WebSocketOpcode::kPong;
  }
};

NEI_SUPPRESS_MSC_WARNING_4251_END

} // namespace nei::net::websocket

#endif // NEIXX_NET_WEBSOCKET_WEBSOCKET_FRAME_H_
