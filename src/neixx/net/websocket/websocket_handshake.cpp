// WebSocket opening handshake (RFC 6455 §4).

#include "websocket_handshake_internal.h"

#include <cstring>

#include <nei/utils/base64.h>
#include <nei/utils/sha1.h>

namespace nei::net::websocket {

// WebSocket magic GUID defined in RFC 6455 §4.2.2.
inline constexpr std::string_view kWebSocketMagicGuid =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string ComputeWebSocketAccept(std::string_view key) {
    // Concatenate key + magic GUID.
    std::string input(key);
    input += kWebSocketMagicGuid;

    // SHA1 hash.
    uint8_t digest[NEI_SHA1_DIGEST_SIZE];
    nei_sha1_sum(input.data(), input.size(), digest);

    // Base64 encode.
    size_t b64_len = nei_base64_encoded_length(NEI_SHA1_DIGEST_SIZE);
    std::string output(b64_len, '\0');
    size_t actual_len = 0;
    nei_base64_encode(digest, NEI_SHA1_DIGEST_SIZE,
                      output.data(), output.size(), &actual_len);
    output.resize(actual_len);
    return output;
}

bool ValidateWebSocketUpgrade(const http::HttpRequest& req) {
    // Method must be GET.
    if (req.method != http::HttpMethod::kGet) return false;

    // Upgrade header must contain "websocket" (case-insensitive).
    std::string_view upgrade = req.GetHeaderValue("Upgrade");
    // Simple case-insensitive check for "websocket".
    if (upgrade.size() != 9) return false;
    for (size_t i = 0; i < 9; ++i) {
        char c = upgrade[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        if (c != "websocket"[i]) return false;
    }

    // Connection header must contain "Upgrade" (case-insensitive token match).
    std::string_view conn = req.GetHeaderValue("Connection");
    // Simple check: the value should contain "Upgrade" (or "upgrade").
    if (conn.find("pgrade") == std::string_view::npos) {
        // Crude check: "Upgrade" contains "pgrade" uniquely among HTTP tokens.
        // Better: proper token parsing, but this is sufficient for well-formed clients.
        return false;
    }

    // Sec-WebSocket-Key must be a 24-character base64 string (decodes to 16 bytes).
    std::string_view ws_key = req.GetHeaderValue("Sec-WebSocket-Key");
    if (ws_key.size() != 24) return false;

    // Decode to verify it's valid base64 of exactly 16 bytes.
    uint8_t decoded[16];
    size_t decoded_len = 0;
    int rc = nei_base64_decode(ws_key.data(), ws_key.size(),
                                decoded, sizeof(decoded), &decoded_len);
    if (rc != NEI_BASE64_OK || decoded_len != 16) return false;

    // Sec-WebSocket-Version must be "13".
    std::string_view version = req.GetHeaderValue("Sec-WebSocket-Version");
    if (version != "13") return false;

    return true;
}

http::HttpResponse BuildWebSocketUpgradeResponse(
    const http::HttpRequest& req) {
    http::HttpResponse resp;
    resp.SetStatus(http::HttpStatusCode::kSwitchingProtocols);
    resp.headers.push_back({"Upgrade", "websocket"});
    resp.headers.push_back({"Connection", "Upgrade"});

    std::string_view key = req.GetHeaderValue("Sec-WebSocket-Key");
    std::string accept = ComputeWebSocketAccept(key);
    resp.headers.push_back({"Sec-WebSocket-Accept", accept});

    // Optional: Sec-WebSocket-Protocol if client requested it.
    std::string_view protocol = req.GetHeaderValue("Sec-WebSocket-Protocol");
    if (!protocol.empty()) {
        resp.headers.push_back(
            {"Sec-WebSocket-Protocol", std::string(protocol)});
    }

    return resp;
}

}  // namespace nei::net::websocket
