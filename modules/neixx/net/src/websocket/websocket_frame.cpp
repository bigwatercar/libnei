// WebSocket data framing (RFC 6455 §5).

#include <neixx/net/websocket/websocket_frame.h>
#include "websocket_frame_internal.h"

#include <algorithm>
#include <cstring>

namespace nei::net::websocket {

// ===========================================================================
// WebSocketFrameParser
// ===========================================================================

WebSocketFrameParser::WebSocketFrameParser() = default;
WebSocketFrameParser::~WebSocketFrameParser() = default;

int64_t WebSocketFrameParser::Parse(const uint8_t *data, size_t len) {
  if (state_ == State::kError)
    return -1;
  if (state_ == State::kComplete)
    return 0; // Caller must Reset() first.

  size_t consumed = 0;
  const uint8_t *p = data;

  while (consumed < len && state_ != State::kComplete && state_ != State::kError) {
    switch (state_) {
    case State::kHeader: {
      if (len - consumed < 2)
        return static_cast<int64_t>(consumed);

      uint8_t byte0 = p[0];
      uint8_t byte1 = p[1];

      frame_.fin = (byte0 & 0x80) != 0;
      frame_.opcode = static_cast<WebSocketOpcode>(byte0 & 0x0F);
      frame_.masked = (byte1 & 0x80) != 0;
      uint8_t payload_len7 = byte1 & 0x7F;

      p += 2;
      consumed += 2;

      if (payload_len7 < 126) {
        frame_.payload.resize(payload_len7);
        payload_read_ = 0;
        state_ = frame_.masked ? State::kMaskingKey : State::kPayload;
      } else if (payload_len7 == 126) {
        state_ = State::kExtendedLen16;
      } else {
        state_ = State::kExtendedLen64;
      }
      break;
    }

    case State::kExtendedLen16: {
      if (len - consumed < 2)
        return static_cast<int64_t>(consumed);

      uint16_t ext_len = (static_cast<uint16_t>(p[0]) << 8) | p[1];
      p += 2;
      consumed += 2;

      frame_.payload.resize(ext_len);
      payload_read_ = 0;
      state_ = frame_.masked ? State::kMaskingKey : State::kPayload;
      break;
    }

    case State::kExtendedLen64: {
      if (len - consumed < 8)
        return static_cast<int64_t>(consumed);

      uint64_t ext_len = 0;
      for (int i = 0; i < 8; ++i) {
        ext_len = (ext_len << 8) | p[i];
      }
      p += 8;
      consumed += 8;

      // For practical reasons, limit to size_t.
      if (ext_len > static_cast<uint64_t>(SIZE_MAX)) {
        state_ = State::kError;
        return -1;
      }
      frame_.payload.resize(static_cast<size_t>(ext_len));
      payload_read_ = 0;
      state_ = frame_.masked ? State::kMaskingKey : State::kPayload;
      break;
    }

    case State::kMaskingKey: {
      if (len - consumed < 4)
        return static_cast<int64_t>(consumed);

      frame_.masking_key.assign(p, p + 4);
      p += 4;
      consumed += 4;
      state_ = State::kPayload;
      break;
    }

    case State::kPayload: {
      size_t remaining = frame_.payload.size() - payload_read_;
      size_t to_copy = std::min(remaining, len - consumed);

      std::memcpy(frame_.payload.data() + payload_read_, p, to_copy);
      payload_read_ += to_copy;
      p += to_copy;
      consumed += to_copy;

      if (payload_read_ == frame_.payload.size()) {
        // Unmask if needed.
        if (frame_.masked && !frame_.masking_key.empty()) {
          WebSocketFrameBuilder::ApplyMask(frame_.masking_key.data(), frame_.payload.data(), frame_.payload.size());
        }
        state_ = State::kComplete;
      }
      break;
    }

    default:
      break;
    }
  }

  return static_cast<int64_t>(consumed);
}

bool WebSocketFrameParser::is_frame_complete() const {
  return state_ == State::kComplete;
}

const WebSocketFrame &WebSocketFrameParser::frame() const {
  return frame_;
}

void WebSocketFrameParser::Reset() {
  state_ = State::kHeader;
  frame_ = WebSocketFrame{};
  payload_read_ = 0;
}

// ===========================================================================
// WebSocketFrameBuilder
// ===========================================================================

std::vector<uint8_t>
WebSocketFrameBuilder::Build(bool fin, WebSocketOpcode opcode, bool masked, const void *payload, size_t payload_len) {

  // Control frames max 125 bytes.
  if ((opcode == WebSocketOpcode::kClose || opcode == WebSocketOpcode::kPing || opcode == WebSocketOpcode::kPong)
      && payload_len > 125) {
    payload_len = 125;
  }

  std::vector<uint8_t> frame;
  size_t header_size = 2; // minimum

  if (payload_len < 126) {
    // Header stays at 2 bytes.
  } else if (payload_len <= 0xFFFF) {
    header_size += 2; // 16-bit extended length.
  } else {
    header_size += 8; // 64-bit extended length.
  }

  if (masked) {
    header_size += 4; // masking key.
  }

  frame.resize(header_size + payload_len);

  uint8_t *p = frame.data();

  // Byte 0: FIN + opcode.
  p[0] = static_cast<uint8_t>(opcode);
  if (fin)
    p[0] |= 0x80;

  // Byte 1: MASK + payload length.
  if (payload_len < 126) {
    p[1] = static_cast<uint8_t>(payload_len);
  } else if (payload_len <= 0xFFFF) {
    p[1] = 126;
    p[2] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(payload_len & 0xFF);
  } else {
    p[1] = 127;
    for (int i = 0; i < 8; ++i) {
      p[2 + i] = static_cast<uint8_t>((payload_len >> (56 - i * 8)) & 0xFF);
    }
  }

  if (masked) {
    p[1] |= 0x80;

    // Masking key sits at header_size - 4, payload at header_size.
    uint8_t *mk_ptr = p + (header_size - 4);

    // Generate deterministic masking key.
    uint32_t seed = 0;
    for (size_t i = 0; i < payload_len && i < 32; ++i) {
      seed = seed * 31 + static_cast<const uint8_t *>(payload)[i];
    }
    mk_ptr[0] = static_cast<uint8_t>((seed >> 24) & 0xFF);
    mk_ptr[1] = static_cast<uint8_t>((seed >> 16) & 0xFF);
    mk_ptr[2] = static_cast<uint8_t>((seed >> 8) & 0xFF);
    mk_ptr[3] = static_cast<uint8_t>(seed & 0xFF);

    // Copy and mask payload.
    uint8_t *payload_ptr = p + header_size;
    std::memcpy(payload_ptr, payload, payload_len);
    ApplyMask(mk_ptr, payload_ptr, payload_len);
  } else {
    // Copy payload directly.
    std::memcpy(p + header_size, payload, payload_len);
  }

  return frame;
}

std::vector<uint8_t> WebSocketFrameBuilder::BuildClose(uint16_t code, std::string_view reason) {
  // Close frame payload: [2-byte status code] [optional reason].
  size_t reason_len = std::min(reason.size(), size_t(123));
  std::vector<uint8_t> close_payload(2 + reason_len);
  close_payload[0] = static_cast<uint8_t>((code >> 8) & 0xFF);
  close_payload[1] = static_cast<uint8_t>(code & 0xFF);
  if (reason_len > 0) {
    std::memcpy(close_payload.data() + 2, reason.data(), reason_len);
  }
  return Build(true, WebSocketOpcode::kClose, false, close_payload.data(), close_payload.size());
}

std::vector<uint8_t> WebSocketFrameBuilder::BuildPing(const void *payload, size_t len) {
  if (len > 125)
    len = 125;
  return Build(true, WebSocketOpcode::kPing, false, payload, len);
}

std::vector<uint8_t> WebSocketFrameBuilder::BuildPong(const void *payload, size_t len) {
  if (len > 125)
    len = 125;
  return Build(true, WebSocketOpcode::kPong, false, payload, len);
}

void WebSocketFrameBuilder::ApplyMask(const uint8_t masking_key[4], uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    data[i] ^= masking_key[i % 4];
  }
}

} // namespace nei::net::websocket
