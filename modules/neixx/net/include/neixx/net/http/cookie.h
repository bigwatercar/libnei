// =============================================================================
// cookie.h — HTTP cookie (RFC 6265) parsing, matching and storage
// =============================================================================
//
// Minimal-but-correct cookie support for the HTTP client:
//   - Cookie::Parse: turns one "Set-Cookie" header value (plus the request
//     URL that carried it) into a Cookie with RFC 6265 defaults (domain,
//     path, expiry).
//   - Cookie::Matches: decides whether a stored cookie is sent with a given
//     request URL (domain-match, path-match, Secure, not expired).
//   - CookieJar: stores cookies and builds the "Cookie" header for a request.
//
// Deliberately a plain component: it does not talk to HttpClient directly.
// Wire it up by calling CookieJar::GetCookieHeader before sending and
// CookieJar::SetCookie for every Set-Cookie header in a response.
//
// Scope notes (kept simple, matching RFC 6265 §5):
//   - Host-only cookies (no Domain attribute) are not sent to subdomains.
//   - Domain attribute must be a suffix of the request host (no public-suffix
//     list; "example.com" matching is accepted for any suffix, as the RFC's
//     canonical rules do without PSL).
//   - SameSite / SameParty / Priority attributes are parsed and ignored.
//   - Session cookies (no Max-Age/Expires) do not persist across the jar's
//     lifetime (no serialization to disk in this iteration).
//
// Thread safety: CookieJar is not thread-safe; guard it externally or keep
// it bound to one request flow at a time.

#ifndef NEIXX_NET_HTTP_COOKIE_H_
#define NEIXX_NET_HTTP_COOKIE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/common/time.h>
#include <neixx/url/url.h>

namespace nei::net::http {

NEI_SUPPRESS_MSC_WARNING_4251_BEGIN

// One RFC 6265 cookie.
class NEI_API Cookie {
public:
  Cookie() = default;

  // Parses one Set-Cookie header value.  |request_url| is the URL of the
  // response that carried the header (used for the domain/path defaults and
  // the Secure semantics).  Returns nullopt when the value is malformed
  // (empty name, invalid domain, etc.).
  static std::optional<Cookie> Parse(std::string_view set_cookie_value, const Url &request_url);

  // "name=value" for the Cookie request header (empty value serialized as
  // "name=").
  std::string ToCookiePair() const;

  // RFC 6265 §5.4 domain-match + path-match + Secure + expiry check against
  // |request_url| at wall-clock |now|.
  bool Matches(const Url &request_url, Time now) const;

  // True when the cookie has a Max-Age/Expires and |now| is past it.
  bool IsExpired(Time now) const;

  // ---- fields ----
  std::string name;
  std::string value;
  // Canonical cookie domain: lowercase, no leading dot (host-only cookies
  // store the exact request host).
  std::string domain;
  // Canonical cookie path (always starts with '/').
  std::string path;
  // Absent = session cookie (no expiry).
  std::optional<Time> expires;
  bool secure = false;
  bool http_only = false;
  bool host_only = true;
};

// A simple in-memory cookie jar.
class NEI_API CookieJar {
public:
  // Stores |cookie|, replacing any existing cookie with the same
  // (name, domain, path).  Expired cookies are dropped.
  void SetCookie(const Cookie &cookie);

  // Builds the "Cookie" request header value for |request_url|:
  // "name=value; name=value", or empty when nothing matches.  Expired
  // cookies are purged lazily during the scan.
  std::string GetCookieHeader(const Url &request_url);

  // Removes all cookies.
  void Clear();

  std::size_t size() const {
    return cookies_.size();
  }

private:
  std::vector<Cookie> cookies_;
};

NEI_SUPPRESS_MSC_WARNING_4251_END

} // namespace nei::net::http

#endif // NEIXX_NET_HTTP_COOKIE_H_
