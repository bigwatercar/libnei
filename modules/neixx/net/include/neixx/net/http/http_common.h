#pragma once

#ifndef NEIXX_NET_HTTP_HTTP_COMMON_H_
#define NEIXX_NET_HTTP_HTTP_COMMON_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>

// =============================================================================
// HttpMethod / HttpStatusCode — HTTP protocol constants
// =============================================================================
//
// These types mirror the llhttp enums (llhttp_method_t / llhttp_status) but
// live in the nei::net::http namespace so consumers are not exposed to the
// underlying C parser.
//
// The enum values are deliberately aligned with llhttp so conversion is a
// simple static_cast.

namespace nei::net::http {

// ---------------------------------------------------------------------------
// HttpMethod — RFC 7231 / RFC 5789 request methods
// ---------------------------------------------------------------------------
enum class HttpMethod : uint8_t {
    kDelete = 0,
    kGet = 1,
    kHead = 2,
    kPost = 3,
    kPut = 4,
    kConnect = 5,
    kOptions = 6,
    kTrace = 7,
    kPatch = 28,
    // Sentinel / unknown.
    kUnknown = 255,
};

// Number of recognized method enum values (including kUnknown).
constexpr int kHttpMethodCount = 10;

// Human-readable method name, e.g. HttpMethod::kGet → "GET".
// Returns "UNKNOWN" for kUnknown or unrecognized values.
NEI_API const char* HttpMethodToString(HttpMethod method);

// Case-insensitive parse.  "get" / "GET" / "Get" → HttpMethod::kGet.
// Returns HttpMethod::kUnknown on unrecognized input.
NEI_API HttpMethod StringToHttpMethod(const char* method, size_t length);

// ---------------------------------------------------------------------------
// HttpStatusCode — RFC 7231 status codes
// ---------------------------------------------------------------------------
// Only the most common codes are enumerated; the raw int value is preserved
// for codes that haven't been explicitly listed.
enum class HttpStatusCode : uint16_t {
    // 1xx Informational
    kContinue = 100,
    kSwitchingProtocols = 101,

    // 2xx Success
    kOk = 200,
    kCreated = 201,
    kAccepted = 202,
    kNoContent = 204,
    kPartialContent = 206,

    // 3xx Redirection
    kMovedPermanently = 301,
    kFound = 302,
    kSeeOther = 303,
    kNotModified = 304,
    kTemporaryRedirect = 307,
    kPermanentRedirect = 308,

    // 4xx Client Error
    kBadRequest = 400,
    kUnauthorized = 401,
    kForbidden = 403,
    kNotFound = 404,
    kMethodNotAllowed = 405,
    kRequestTimeout = 408,
    kConflict = 409,
    kGone = 410,
    kLengthRequired = 411,
    kPayloadTooLarge = 413,
    kUriTooLong = 414,
    kUnsupportedMediaType = 415,
    kTooManyRequests = 429,

    // 5xx Server Error
    kInternalServerError = 500,
    kNotImplemented = 501,
    kBadGateway = 502,
    kServiceUnavailable = 503,
    kGatewayTimeout = 504,
};

// Human-readable status text, e.g. HttpStatusCode::kNotFound → "Not Found".
// Falls back to "Unknown Status" for unrecognized codes.
NEI_API const char* HttpStatusCodeToString(HttpStatusCode code);

// ---------------------------------------------------------------------------
// HttpStatus — a single HTTP status value (enum view + raw numeric view)
// ---------------------------------------------------------------------------
// Wraps HttpStatusCode with a guaranteed-consistent raw numeric view.  Both
// code() and raw_code() are derived from one stored value, so they can never
// diverge.  Non-enumerated codes (e.g. 599) are supported via FromRaw().
struct NEI_API HttpStatus {
    // Default: HTTP 200 OK.
    HttpStatus() = default;

    // From an enumerated code; raw_code() == static_cast<int>(code).
    /* implicit */ HttpStatus(HttpStatusCode code) : raw_code_(static_cast<int>(code)) {}

    // From a raw numeric code (which may lack an enumerated HttpStatusCode,
    // e.g. 599).  code() returns the enum holding the same numeric value.
    static HttpStatus FromRaw(int raw_code) {
        HttpStatus s;
        s.raw_code_ = raw_code;
        return s;
    }

    // Enumerated view (best-effort for non-enumerated raw codes).
    HttpStatusCode code() const { return static_cast<HttpStatusCode>(raw_code_); }
    // Exact numeric wire code.
    int raw_code() const { return raw_code_; }

    // Set from an enumerated code.
    void SetStatus(HttpStatusCode code) { raw_code_ = static_cast<int>(code); }
    // Set from a raw numeric code (may be non-enumerated, e.g. 599).
    void SetRawStatus(int raw_code) { raw_code_ = raw_code; }

    bool operator==(const HttpStatus &o) const { return raw_code_ == o.raw_code_; }
    bool operator!=(const HttpStatus &o) const { return !(*this == o); }

private:
    // Single source of truth: the exact numeric wire code.  code() derives
    // the enum view (the enum's underlying type is uint16_t, so any valid
    // HTTP code 100-599 fits; non-enumerated codes simply hold their value).
    int raw_code_ = 200;
};

// ---------------------------------------------------------------------------
// HttpHeader — a single name-value header pair
// ---------------------------------------------------------------------------
NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
struct NEI_API HttpHeader {
    std::string name;
    std::string value;

    HttpHeader() = default;
    HttpHeader(std::string n, std::string v) : name(std::move(n)), value(std::move(v)) {}

    bool operator==(const HttpHeader& other) const { return name == other.name && value == other.value; }
    bool operator!=(const HttpHeader& other) const { return !(*this == other); }
};
NEI_SUPPRESS_MSC_WARNING_4251_END

using HttpHeaders = std::vector<HttpHeader>;

// Route parameters extracted from URL path patterns (e.g. /user/:id).
using RouteParams = std::unordered_map<std::string, std::string>;

// ---------------------------------------------------------------------------
// HttpVersion — HTTP protocol version
// ---------------------------------------------------------------------------
enum class HttpVersion : uint8_t {
    kHttp10 = 0,  // HTTP/1.0
    kHttp11 = 1,  // HTTP/1.1
    kUnknown = 255,
};

NEI_API const char* HttpVersionToString(HttpVersion version);

}  // namespace nei::net::http

#endif  // NEIXX_NET_HTTP_HTTP_COMMON_H_
