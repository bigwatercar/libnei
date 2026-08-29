#include <neixx/net/http/http_request.h>
#include <neixx/strings/string_util.h>

#include <algorithm>
#include <cctype>

namespace nei::net::http {

const HttpHeader* HttpRequest::FindHeader(std::string_view name) const {
    for (const auto& h : headers) {
        if (EqualsCaseInsensitiveASCII(h.name, name)) return &h;
    }
    return nullptr;
}

std::string_view HttpRequest::GetHeaderValue(std::string_view name) const {
    const HttpHeader* h = FindHeader(name);
    return h ? std::string_view(h->value) : std::string_view();
}

bool HttpRequest::keep_alive() const {
    if (http_version == HttpVersion::kHttp10) {
        // HTTP/1.0: keep-alive only if explicitly requested.
        auto val = GetHeaderValue("Connection");
        return EqualsCaseInsensitiveASCII(val, "keep-alive");
    }
    // HTTP/1.1: keep-alive unless explicitly closed.
    auto val = GetHeaderValue("Connection");
    return !EqualsCaseInsensitiveASCII(val, "close");
}

void HttpRequest::Clear() {
    method = HttpMethod::kUnknown;
    url = Url();
    headers.clear();
    body.clear();
    http_version = HttpVersion::kUnknown;
    route_params.clear();
}

bool HttpRequest::operator==(const HttpRequest& other) const {
    return method == other.method &&
           url == other.url &&
           headers == other.headers &&
           body == other.body &&
           http_version == other.http_version;
}

}  // namespace nei::net::http
