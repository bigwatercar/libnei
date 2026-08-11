// RFC 3986 URL parser implementation.
// Handles: scheme, authority (user:pass@host:port), path, query, fragment.
// Supports IPv6 literal hosts and WHATWG default port logic for http/ws/ftp.

#include <neixx/url/url.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nei {

// =============================================================================
// Internal helpers
// =============================================================================

namespace {

constexpr bool IsAlpha(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

constexpr bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}

constexpr bool IsSchemeChar(char c) {
  return IsAlpha(c) || IsDigit(c) || c == '+' || c == '-' || c == '.';
}

constexpr bool IsUnreserved(char c) {
  return IsAlpha(c) || IsDigit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

constexpr bool IsSubDelim(char c) {
  return c == '!' || c == '$' || c == '&' || c == '\'' || c == '(' || c == ')' || c == '*' || c == '+' || c == ','
         || c == ';' || c == '=';
}

// Sub-delimiters + ":" + "@" are allowed in userinfo.
constexpr bool IsUserInfoChar(char c) {
  return IsUnreserved(c) || IsSubDelim(c) || c == ':';
}

// Hex digit to value; returns -1 for invalid.
int HexValue(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

// Default port numbers per scheme (IANA / WHATWG).
std::uint16_t DefaultPortForScheme(std::string_view scheme) {
  if (scheme == "http" || scheme == "ws")
    return 80;
  if (scheme == "https" || scheme == "wss")
    return 443;
  if (scheme == "ftp")
    return 21;
  if (scheme == "ssh")
    return 22;
  return 0;
}

// Remove "." and ".." segments from a path (RFC 3986 §5.2.4).
std::string NormalizePathSegments(std::string_view path) {
  if (path.empty())
    return "/";

  std::vector<std::string_view> segments;
  std::string_view p = path;
  if (!p.empty() && p[0] == '/')
    p.remove_prefix(1);

  // Split.
  while (!p.empty()) {
    auto slash = p.find('/');
    auto seg = (slash == std::string_view::npos) ? p : p.substr(0, slash);
    if (seg == "..") {
      if (!segments.empty())
        segments.pop_back();
    } else if (seg != "." && !seg.empty()) {
      segments.push_back(seg);
    }
    if (slash == std::string_view::npos)
      break;
    p = p.substr(slash + 1);
  }

  // Rebuild.
  std::string result;
  result.reserve(path.size());
  if (path.empty() || path[0] == '/')
    result += '/';
  for (std::size_t i = 0; i < segments.size(); ++i) {
    if (i > 0)
      result += '/';
    result.append(segments[i]);
  }
  // Preserve trailing slash if original had one (for non-empty path).
  if (!path.empty() && path.back() == '/' && !result.empty() && result.back() != '/')
    result += '/';
  return result;
}

// Parse an IPv6 literal host enclosed in brackets: "[::1]".
// Returns the position after the closing bracket, or npos on failure.
std::size_t EatIPv6Literal(std::string_view s, std::size_t pos) {
  if (pos >= s.size() || s[pos] != '[')
    return std::string_view::npos;
  auto close = s.find(']', pos);
  if (close == std::string_view::npos)
    return std::string_view::npos;
  return close + 1;
}

// Parse an IPv4 or registered-name host.
std::size_t EatHost(std::string_view s, std::size_t pos) {
  while (pos < s.size() && s[pos] != ':' && s[pos] != '/' && s[pos] != '?' && s[pos] != '#')
    ++pos;
  return pos;
}

} // namespace

// =============================================================================
// Url::Impl
// =============================================================================

struct Url::Impl {
  std::string spec;

  // Component boundaries as offsets into |spec|.
  std::size_t scheme_begin = 0, scheme_end = 0;
  std::size_t user_begin = 0, user_end = 0;
  std::size_t pass_begin = 0, pass_end = 0;
  std::size_t host_begin = 0, host_end = 0;
  std::size_t port_begin = 0, port_end = 0;
  std::size_t path_begin = 0, path_end = 0;
  std::size_t query_begin = 0, query_end = 0;
  std::size_t frag_begin = 0, frag_end = 0;

  std::uint16_t port_value = 0;
  bool port_explicit = false;
  bool valid = false;

  std::string origin_cache;

  void Parse();
  void Reset();
};

void Url::Impl::Reset() {
  // Keep |spec| intact — it's the source of all component string_views.
  scheme_begin = scheme_end = 0;
  user_begin = user_end = 0;
  pass_begin = pass_end = 0;
  host_begin = host_end = 0;
  port_begin = port_end = 0;
  path_begin = path_end = 0;
  query_begin = query_end = 0;
  frag_begin = frag_end = 0;
  port_value = 0;
  port_explicit = false;
  valid = false;
  origin_cache.clear();
}

void Url::Impl::Parse() {
  // Snapshot the spec BEFORE Reset() clears member offsets.
  // Reset() zeros all component boundaries but preserves |spec|.
  std::string_view s = spec;
  Reset();

  if (s.empty())
    return;

  std::size_t pos = 0;

  // ---- Scheme ----
  std::size_t scheme_start = pos;
  while (pos < s.size() && IsSchemeChar(s[pos]))
    ++pos;

  if (pos > scheme_start && pos < s.size() && s[pos] == ':') {
    scheme_begin = scheme_start;
    scheme_end = pos;
    // Convert scheme to lowercase.
    for (std::size_t i = scheme_begin; i < scheme_end; ++i)
      spec[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(spec[i])));
    ++pos; // skip ':'
  } else {
    // No scheme — treat as relative reference (path, query, or fragment).
    if (!s.empty() && s[0] == '?') {
      query_begin = 1;
      query_end = s.size();
    } else if (!s.empty() && s[0] == '#') {
      frag_begin = 1;
      frag_end = s.size();
    } else {
      path_begin = 0;
      path_end = s.size();
    }
    valid = true;
    return;
  }

  // ---- Authority (after "//") ----
  if (pos + 1 < s.size() && s[pos] == '/' && s[pos + 1] == '/') {
    pos += 2; // skip "//"

    std::size_t authority_start = pos;

    // Scan for end of authority (@, /, ?, #, or EOS).
    auto auth_end = s.find_first_of("/?#", pos);
    if (auth_end == std::string_view::npos)
      auth_end = s.size();

    // Check for userinfo (contains '@').
    auto at_sign = s.find('@', pos);
    if (at_sign != std::string_view::npos && at_sign < auth_end) {
      // user:password@
      user_begin = pos;
      auto colon = s.find(':', pos);
      if (colon != std::string_view::npos && colon < at_sign) {
        user_end = colon;
        pass_begin = colon + 1;
        pass_end = at_sign;
      } else {
        user_end = at_sign;
      }
      pos = at_sign + 1;
    }

    // Host (IPv6 literal or registered name).
    if (pos < auth_end && s[pos] == '[') {
      auto close = EatIPv6Literal(s, pos);
      if (close != std::string_view::npos) {
        host_begin = pos;
        host_end = close;
        pos = close;
      }
    } else {
      host_begin = pos;
      host_end = EatHost(s, pos);
      pos = host_end;
    }

    // Lowercase host (RFC 3986 §6.2.2.1).
    if (host_begin < host_end) {
      for (std::size_t i = host_begin; i < host_end; ++i)
        spec[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(spec[i])));
    }

    // Port.
    if (pos < auth_end && s[pos] == ':') {
      ++pos; // skip ':'
      port_begin = pos;
      port_end = EatHost(s, pos); // digits
      std::string_view port_str = std::string_view(spec).substr(port_begin, port_end - port_begin);
      char *end = nullptr;
      long p = std::strtol(port_str.data(), &end, 10);
      if (end == port_str.data() + port_str.size() && p > 0 && p <= 65535) {
        port_value = static_cast<std::uint16_t>(p);
        port_explicit = true;
      }
      pos = port_end;
    }

    // If host was unspecified, this is an opaque scheme (e.g. "mailto:").
    if (host_begin >= host_end) {
      host_begin = host_end = 0;
    }
  }

  // ---- Path ----
  if (pos < s.size()) {
    path_begin = pos;
    path_end = s.size(); // tentative
  }

  // ---- Query ----
  auto q = s.find('?', path_begin);
  if (q != std::string_view::npos) {
    path_end = q;
    query_begin = q + 1;
    query_end = s.size(); // tentative
  }

  // ---- Fragment ----
  auto f = s.find('#', query_begin > 0 ? query_begin : path_begin);
  if (f != std::string_view::npos) {
    if (query_begin > 0)
      query_end = f;
    else
      path_end = f;
    frag_begin = f + 1;
    frag_end = s.size();
  }

  // Default port.
  if (!port_explicit) {
    std::string_view s = std::string_view(spec).substr(scheme_begin, scheme_end - scheme_begin);
    port_value = DefaultPortForScheme(s);
  }

  valid = true;
}

// =============================================================================
// Url public API
// =============================================================================

Url::Url()
    : impl_(new Impl) {
}

Url::Url(std::string url)
    : impl_(new Impl) {
  impl_->spec = std::move(url);
  impl_->Parse();
}

Url::~Url() {
  delete impl_;
}

Url::Url(const Url &other)
    : impl_(new Impl(*other.impl_)) {
}

Url &Url::operator=(const Url &other) {
  *impl_ = *other.impl_;
  return *this;
}

Url::Url(Url &&other) noexcept
    : impl_(other.impl_) {
  other.impl_ = nullptr;
}

Url &Url::operator=(Url &&other) noexcept {
  delete impl_;
  impl_ = other.impl_;
  other.impl_ = nullptr;
  return *this;
}

std::string_view Url::scheme() const {
  return impl_->scheme_begin < impl_->scheme_end
             ? std::string_view(impl_->spec).substr(impl_->scheme_begin, impl_->scheme_end - impl_->scheme_begin)
             : std::string_view{};
}

std::string_view Url::user() const {
  return impl_->user_begin < impl_->user_end
             ? std::string_view(impl_->spec).substr(impl_->user_begin, impl_->user_end - impl_->user_begin)
             : std::string_view{};
}

std::string_view Url::password() const {
  return impl_->pass_begin < impl_->pass_end
             ? std::string_view(impl_->spec).substr(impl_->pass_begin, impl_->pass_end - impl_->pass_begin)
             : std::string_view{};
}

std::string_view Url::host() const {
  return impl_->host_begin < impl_->host_end
             ? std::string_view(impl_->spec).substr(impl_->host_begin, impl_->host_end - impl_->host_begin)
             : std::string_view{};
}

std::uint16_t Url::port() const {
  return impl_->port_value;
}

std::string_view Url::path() const {
  return impl_->path_begin < impl_->path_end
             ? std::string_view(impl_->spec).substr(impl_->path_begin, impl_->path_end - impl_->path_begin)
             : std::string_view{};
}

std::string_view Url::query() const {
  return impl_->query_begin < impl_->query_end
             ? std::string_view(impl_->spec).substr(impl_->query_begin, impl_->query_end - impl_->query_begin)
             : std::string_view{};
}

std::string_view Url::fragment() const {
  return impl_->frag_begin < impl_->frag_end
             ? std::string_view(impl_->spec).substr(impl_->frag_begin, impl_->frag_end - impl_->frag_begin)
             : std::string_view{};
}

bool Url::is_valid() const {
  return impl_->valid;
}

bool Url::is_empty() const {
  return impl_->spec.empty();
}

const std::string &Url::spec() const {
  return impl_->spec;
}

std::string Url::origin() const {
  if (impl_->spec.empty() || scheme().empty() || host().empty())
    return {};

  if (!impl_->origin_cache.empty())
    return impl_->origin_cache;

  std::ostringstream oss;
  oss << scheme() << "://";
  auto h = host();
  oss << h;
  auto dp = DefaultPortForScheme(scheme());
  if (impl_->port_explicit && impl_->port_value != dp)
    oss << ':' << impl_->port_value;
  impl_->origin_cache = oss.str();
  return impl_->origin_cache;
}

Url Url::Resolve(std::string_view relative) const {
  if (relative.empty())
    return *this;

  Url ref{std::string(relative)};
  if (!ref.is_valid())
    return *this;

  // RFC 3986 §5.2.2 — relative reference resolution.

  // If ref has a scheme, it replaces everything.
  if (!ref.scheme().empty())
    return ref;

  // If ref has authority, take base scheme + ref authority + ref path/query.
  if (!ref.host().empty()) {
    std::string result;
    std::string_view ref_spec = ref.spec();
    result.reserve(scheme().size() + 3 + ref_spec.size());
    result.append(scheme());
    result += "://";
    result.append(ref_spec.substr(ref.impl_->host_begin));
    return Url(std::move(result));
  }

  // No scheme, no authority — merge paths.
  std::string_view ref_path = ref.path();
  if (ref_path.empty()) {
    // Reference is query-only or fragment-only.
    std::string result = impl_->spec.substr(0, impl_->path_end);
    std::string_view ref_query = ref.query();
    std::string_view ref_frag = ref.fragment();
    if (!ref_query.empty()) {
      result += '?';
      result.append(ref_query);
    }
    if (!ref_frag.empty()) {
      result += '#';
      result.append(ref_frag);
    }
    return Url(std::move(result));
  }

  std::string result;
  std::string_view ref_spec = ref.spec();

  if (ref_path[0] == '/') {
    // Absolute path — replace base path.
    result.reserve(scheme().size() + 3 + host().size() + ref_spec.size());
    result.append(scheme());
    result += "://";
    result.append(host());
    if (impl_->port_explicit && impl_->port_value != DefaultPortForScheme(scheme())) {
      result += ':';
      result.append(std::to_string(impl_->port_value));
    }
    result.append(ref_spec.substr(ref.impl_->path_begin));
    return Url(std::move(result));
  }

  // Relative path — merge with base path.
  std::string base_path_str(impl_->spec.substr(impl_->path_begin, impl_->path_end - impl_->path_begin));
  // Strip everything after the last '/'.
  auto last_slash = base_path_str.rfind('/');
  if (last_slash != std::string::npos)
    base_path_str.resize(last_slash + 1);
  else
    base_path_str = "/";
  base_path_str.append(ref_path);
  base_path_str = NormalizePathSegments(base_path_str);

  result.reserve(scheme().size() + 3 + host().size() + base_path_str.size() + ref_spec.size());
  result.append(scheme());
  result += "://";
  result.append(host());
  if (impl_->port_explicit && impl_->port_value != DefaultPortForScheme(scheme())) {
    result += ':';
    result.append(std::to_string(impl_->port_value));
  }
  result.append(base_path_str);
  std::string_view ref_query = ref.query();
  std::string_view ref_frag = ref.fragment();
  if (!ref_query.empty()) {
    result += '?';
    result.append(ref_query);
  }
  if (!ref_frag.empty()) {
    result += '#';
    result.append(ref_frag);
  }

  return Url(std::move(result));
}

bool operator==(const Url &a, const Url &b) {
  return a.spec() == b.spec();
}

bool operator!=(const Url &a, const Url &b) {
  return !(a == b);
}

std::ostream &operator<<(std::ostream &os, const Url &url) {
  return os << url.spec();
}

} // namespace nei
