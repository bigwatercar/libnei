#include <neixx/net/http/http_response.h>
#include <neixx/strings/string_util.h>

#include <cctype>

namespace nei::net::http {

const HttpHeader* HttpResponse::FindHeader(std::string_view name) const {
    for (const auto& h : headers) {
        if (EqualsCaseInsensitiveASCII(h.name, name)) return &h;
    }
    return nullptr;
}

std::string_view HttpResponse::GetHeaderValue(std::string_view name) const {
    const HttpHeader* h = FindHeader(name);
    return h ? std::string_view(h->value) : std::string_view();
}

bool HttpResponse::keep_alive() const {
    if (http_version == HttpVersion::kHttp10) {
        auto val = GetHeaderValue("Connection");
        return EqualsCaseInsensitiveASCII(val, "keep-alive");
    }
    auto val = GetHeaderValue("Connection");
    return !EqualsCaseInsensitiveASCII(val, "close");
}

void HttpResponse::Clear() {
    status = HttpStatus();
    headers.clear();
    body.clear();
    http_version = HttpVersion::kUnknown;
}

bool HttpResponse::operator==(const HttpResponse& other) const {
    return status == other.status &&
           headers == other.headers &&
           body == other.body &&
           http_version == other.http_version;
}

}  // namespace nei::net::http
