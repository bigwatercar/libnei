#pragma once

#ifndef NEIXX_URL_URL_H_
#define NEIXX_URL_URL_H_

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

#include <nei/build/nei_export.h>

namespace nei {

// =============================================================================
// Url — RFC 3986 compliant URL with zero-copy string storage
// =============================================================================
//
// Stores the original URL string and parsed component offsets (scheme,
// authority, host, port, path, query, fragment).  Does not allocate
// per-component substrings; accessors return std::string_view slices.
//
// Supports:
//   - All standard schemes (http, https, ws, wss, ftp, file, ...)
//   - IPv6 literal hosts (`https://[::1]:8080/path`)
//   - Percent-encoded components (decoded on access)
//   - RFC 3986 relative reference resolution (`base.Resolve("../other")`)
//   - Origin computation (`url.origin()`)
//
// Parsing follows the WHATWG URL Standard for http/https/ws/wss and falls
// back to RFC 3986 for other schemes.
//
// Usage:
//   Url u("https://user:pass@example.com:8080/path?a=1#frag");
//   u.scheme();    // "https"
//   u.host();      // "example.com"
//   u.port();      // 8080  (or default port if omitted)
//   u.path();      // "/path"
//   u.query();     // "a=1"
//   u.fragment();  // "frag"
//   u.origin();    // "https://example.com:8080"

class NEI_API Url {
public:
  Url();
  explicit Url(std::string url);
  ~Url();

  Url(const Url &);
  Url &operator=(const Url &);
  Url(Url &&) noexcept;
  Url &operator=(Url &&) noexcept;

  // ---- Component accessors (zero-copy string_view) ----

  std::string_view scheme() const;
  std::string_view user() const;
  std::string_view password() const;
  std::string_view host() const;
  std::uint16_t port() const;
  std::string_view path() const;
  std::string_view query() const;
  std::string_view fragment() const;

  // ---- Compound queries ----

  bool is_valid() const;
  bool is_empty() const;

  /// Scheme + "://" + host + (":" + port if non-default).
  std::string origin() const;

  /// Full URL string.
  const std::string &spec() const;

  // ---- Relative reference resolution (RFC 3986 §5) ----

  /// Resolve |relative| against this base URL.  Supports absolute paths,
  /// relative paths with ".." / "." segments, query-only and fragment-only
  /// references.
  Url Resolve(std::string_view relative) const;

  // ---- Comparison ----

  friend NEI_API bool operator==(const Url &a, const Url &b);
  friend NEI_API bool operator!=(const Url &a, const Url &b);
  friend NEI_API std::ostream &operator<<(std::ostream &os, const Url &url);

private:
  struct Impl;
  Impl *impl_;
};

} // namespace nei

#endif // NEIXX_URL_URL_H_
