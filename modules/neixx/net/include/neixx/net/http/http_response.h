#pragma once

#ifndef NEIXX_NET_HTTP_HTTP_RESPONSE_H_
#define NEIXX_NET_HTTP_HTTP_RESPONSE_H_

#include <cstdint>
#include <string>
#include <vector>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/net/http/http_common.h>

namespace nei::net::http {

// =============================================================================
// HttpResponse — a parsed HTTP response
// =============================================================================
//
// Holds status code, headers, and body for a single HTTP response message.
//
// Usage:
//   HttpResponse resp;
//   resp.SetStatus(HttpStatusCode::kOk);
//   resp.headers.push_back({"Content-Type", "text/html"});
//   resp.body = "<html>...</html>";

NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
class NEI_API HttpResponse {
public:
    // Single source of truth for the status; code()/raw_code() never diverge.
    HttpStatus status;
    HttpHeaders headers;
    std::string body;
    HttpVersion http_version = HttpVersion::kUnknown;

    HttpResponse() = default;

    // Convenience: set the status from an enumerated code.
    void SetStatus(HttpStatusCode code) { status.SetStatus(code); }
    // Convenience: set the status from a raw numeric code (which may be
    // non-enumerated, e.g. 599).
    void SetRawStatus(int raw_code) { status.SetRawStatus(raw_code); }

    // Convenience: find a header by name (case-insensitive).  Returns nullptr
    // if not found.
    const HttpHeader* FindHeader(std::string_view name) const;

    // Convenience: get header value by name (case-insensitive).  Returns
    // empty string_view if not found.
    std::string_view GetHeaderValue(std::string_view name) const;

    // Returns true if the response indicates keep-alive semantics.
    bool keep_alive() const;

    // Returns true if status is 2xx.
    bool is_success() const {
        int code = status.raw_code();
        return code >= 200 && code < 300;
    }

    // Reset all fields for reuse.
    void Clear();

    bool operator==(const HttpResponse& other) const;
    bool operator!=(const HttpResponse& other) const { return !(*this == other); }
};
NEI_SUPPRESS_MSC_WARNING_4251_END

}  // namespace nei::net::http

#endif  // NEIXX_NET_HTTP_HTTP_RESPONSE_H_
