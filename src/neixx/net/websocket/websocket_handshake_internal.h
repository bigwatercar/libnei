#pragma once

#ifndef NEIXX_NET_WEBSOCKET_WEBSOCKET_HANDSHAKE_INTERNAL_H_
#define NEIXX_NET_WEBSOCKET_WEBSOCKET_HANDSHAKE_INTERNAL_H_

#include <string>
#include <string_view>

#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>

// =============================================================================
// WebSocket opening handshake utilities (RFC 6455 §4) — internal
// =============================================================================
//
// These are implementation helpers for the WebSocket upgrade handshake.
// They are not part of the public API — HttpServer::AddWebSocketRoute()
// handles the upgrade flow automatically.

namespace nei::net::websocket {

// Compute Sec-WebSocket-Accept from the client's Sec-WebSocket-Key.
//   accept = BASE64(SHA1(key + magic_guid))
std::string ComputeWebSocketAccept(std::string_view key);

// Validate that |req| is a well-formed WebSocket upgrade request.
bool ValidateWebSocketUpgrade(const http::HttpRequest& req);

// Build the 101 Switching Protocols response for a valid upgrade request.
http::HttpResponse BuildWebSocketUpgradeResponse(
    const http::HttpRequest& req);

}  // namespace nei::net::websocket

#endif  // NEIXX_NET_WEBSOCKET_WEBSOCKET_HANDSHAKE_INTERNAL_H_
