#pragma once

#ifndef NEIXX_NET_HTTP_HTTP_REQUEST_H_
#define NEIXX_NET_HTTP_HTTP_REQUEST_H_

#include <cstdint>
#include <string>
#include <vector>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/net/http/http_common.h>
#include <neixx/url/url.h>

namespace nei::net::http {

// =============================================================================
// HttpRequest — a parsed HTTP request
// =============================================================================
//
// Holds method, URL, headers, and body for a single HTTP request message.
// This is a value type; copies are shallow (body is shared via string).
//
// Usage:
//   HttpRequest req;
//   req.method = HttpMethod::kGet;
//   req.url = Url("https://example.com/path");
//   req.headers.push_back({"Host", "example.com"});
//   if (req.method == HttpMethod::kPost) {
//       auto& body = req.body;  // std::string
//   }

NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
class NEI_API HttpRequest {
public:
    HttpMethod method = HttpMethod::kUnknown;
    Url url;
    HttpHeaders headers;
    std::string body;
    HttpVersion http_version = HttpVersion::kUnknown;

    // Route parameters extracted from path patterns (e.g. "/user/:id").
    // Populated by HttpServer when a pattern route matches.
    RouteParams route_params;

    HttpRequest() = default;

    // Convenience: find a header by name (case-insensitive).  Returns nullptr
    // if not found.
    const HttpHeader* FindHeader(std::string_view name) const;

    // Convenience: get header value by name (case-insensitive).  Returns
    // empty string_view if not found.
    std::string_view GetHeaderValue(std::string_view name) const;

    // Returns true if the request indicates keep-alive semantics.
    bool keep_alive() const;

    // Reset all fields for reuse.
    void Clear();

    bool operator==(const HttpRequest& other) const;
    bool operator!=(const HttpRequest& other) const { return !(*this == other); }
};
NEI_SUPPRESS_MSC_WARNING_4251_END

}  // namespace nei::net::http

#endif  // NEIXX_NET_HTTP_HTTP_REQUEST_H_
