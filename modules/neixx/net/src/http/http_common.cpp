// HttpMethod ↔ string conversion, HttpStatusCode ↔ string conversion, etc.

#include <neixx/net/http/http_common.h>

#include <cstring>

namespace nei::net::http {

namespace {

struct MethodEntry {
    const char* name;
    HttpMethod method;
};

constexpr MethodEntry kMethodTable[] = {
    {"DELETE",  HttpMethod::kDelete},
    {"GET",     HttpMethod::kGet},
    {"HEAD",    HttpMethod::kHead},
    {"POST",    HttpMethod::kPost},
    {"PUT",     HttpMethod::kPut},
    {"CONNECT", HttpMethod::kConnect},
    {"OPTIONS", HttpMethod::kOptions},
    {"TRACE",   HttpMethod::kTrace},
    {"PATCH",   HttpMethod::kPatch},
};

struct StatusEntry {
    HttpStatusCode code;
    const char* text;
};

constexpr StatusEntry kStatusTable[] = {
    {HttpStatusCode::kContinue,                     "Continue"},
    {HttpStatusCode::kSwitchingProtocols,            "Switching Protocols"},

    {HttpStatusCode::kOk,                            "OK"},
    {HttpStatusCode::kCreated,                       "Created"},
    {HttpStatusCode::kAccepted,                      "Accepted"},
    {HttpStatusCode::kNoContent,                     "No Content"},
    {HttpStatusCode::kPartialContent,                "Partial Content"},

    {HttpStatusCode::kMovedPermanently,              "Moved Permanently"},
    {HttpStatusCode::kFound,                         "Found"},
    {HttpStatusCode::kSeeOther,                      "See Other"},
    {HttpStatusCode::kNotModified,                   "Not Modified"},
    {HttpStatusCode::kTemporaryRedirect,             "Temporary Redirect"},
    {HttpStatusCode::kPermanentRedirect,             "Permanent Redirect"},

    {HttpStatusCode::kBadRequest,                    "Bad Request"},
    {HttpStatusCode::kUnauthorized,                   "Unauthorized"},
    {HttpStatusCode::kForbidden,                     "Forbidden"},
    {HttpStatusCode::kNotFound,                      "Not Found"},
    {HttpStatusCode::kMethodNotAllowed,              "Method Not Allowed"},
    {HttpStatusCode::kRequestTimeout,                "Request Timeout"},
    {HttpStatusCode::kConflict,                      "Conflict"},
    {HttpStatusCode::kGone,                          "Gone"},
    {HttpStatusCode::kLengthRequired,                "Length Required"},
    {HttpStatusCode::kPayloadTooLarge,               "Payload Too Large"},
    {HttpStatusCode::kUriTooLong,                    "URI Too Long"},
    {HttpStatusCode::kUnsupportedMediaType,          "Unsupported Media Type"},
    {HttpStatusCode::kTooManyRequests,               "Too Many Requests"},

    {HttpStatusCode::kInternalServerError,           "Internal Server Error"},
    {HttpStatusCode::kNotImplemented,                "Not Implemented"},
    {HttpStatusCode::kBadGateway,                    "Bad Gateway"},
    {HttpStatusCode::kServiceUnavailable,            "Service Unavailable"},
    {HttpStatusCode::kGatewayTimeout,                "Gateway Timeout"},
};

// Case-insensitive memcmp for method matching.
bool MemEqCi(const char* a, const char* b, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        char ca = a[i];
        char cb = b[i];
        // ASCII lowercasing.
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
        if (ca != cb) return false;
    }
    return true;
}

}  // namespace

const char* HttpMethodToString(HttpMethod method) {
    for (const auto& entry : kMethodTable) {
        if (entry.method == method) return entry.name;
    }
    return "UNKNOWN";
}

HttpMethod StringToHttpMethod(const char* method, size_t length) {
    for (const auto& entry : kMethodTable) {
        size_t nameLen = std::strlen(entry.name);
        if (nameLen == length && MemEqCi(method, entry.name, length)) {
            return entry.method;
        }
    }
    return HttpMethod::kUnknown;
}

const char* HttpStatusCodeToString(HttpStatusCode code) {
    for (const auto& entry : kStatusTable) {
        if (entry.code == code) return entry.text;
    }
    // Handle common codes not in the table via fallback.
    return "Unknown Status";
}

const char* HttpVersionToString(HttpVersion version) {
    switch (version) {
        case HttpVersion::kHttp10: return "HTTP/1.0";
        case HttpVersion::kHttp11: return "HTTP/1.1";
        default:                   return "HTTP/?.?";
    }
}

}  // namespace nei::net::http
