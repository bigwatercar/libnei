// =============================================================================
// cookie.cpp — HTTP cookie (RFC 6265) parsing, matching and storage
// =============================================================================

#include <neixx/net/http/cookie.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace nei::net::http {

namespace {

void TrimWhitespace(std::string *s) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  const auto first = std::find_if(s->begin(), s->end(), not_space);
  const auto last = std::find_if(s->rbegin(), s->rend(), not_space).base();
  if (first >= last) {
    s->clear();
    return;
  }
  s->assign(first, last);
}

std::string ToLower(std::string_view in) {
  std::string out(in);
  for (char &c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Case-insensitive ASCII compare (cookie names/domains).
bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

// Parses an HTTP date (RFC 9110 §5.6.7): IMF-fixdate (RFC 1123) and asctime
// formats.  Returns Unix epoch seconds, or nullopt on failure.  (%*s skips
// the weekday word — including its trailing comma — which avoids the %[^,]
// scan-set quirks across C runtimes.)
std::optional<int64_t> ParseHttpDate(std::string_view text) {
  static constexpr char kMonths[12][4] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  // Shape 1 (RFC 1123): "Sun, 06 Nov 1994 08:49:37 GMT"
  // Shape 2 (asctime):  "Sun Nov  6 08:49:37 1994"
  int day = 0, year = 0, hour = 0, minute = 0, second = 0;
  char month[4] = {0};
  int matched = 0;
  if (std::sscanf(std::string(text).c_str(), "%*s %d %3s %d %d:%d:%d %*s", &day, month, &year, &hour, &minute, &second)
      == 6) {
    matched = 1;
  } else if (std::sscanf(
                 std::string(text).c_str(), "%*s %3s %d %d:%d:%d %d", month, &day, &hour, &minute, &second, &year)
             == 6) {
    matched = 1;
  }
  if (!matched)
    return std::nullopt;

  int mon = -1;
  for (int i = 0; i < 12; ++i) {
    if (std::strncmp(month, kMonths[i], 3) == 0) {
      mon = i;
      break;
    }
  }
  if (mon < 0)
    return std::nullopt;
  if (year < 70) // RFC 850 two-digit year: 70-99 = 19xx, 0-69 = 20xx.
    year += 2000;
  else if (year < 100)
    year += 1900;

  // Days-from-civil (Howard Hinnant).
  const int y = year - (mon <= 1 ? 1 : 0);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (mon + (mon > 1 ? -2 : 10)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = era * 146097 + static_cast<int>(doe) - 719468;
  return days * 86400 + hour * 3600 + minute * 60 + second;
}

// RFC 6265 §5.1.4 default-path algorithm.
std::string DefaultPath(std::string_view request_path) {
  if (request_path.empty() || request_path.front() != '/')
    return "/";
  if (request_path.size() == 1)
    return "/";
  // Drop everything after the last '/', keep that '/' unless it is the
  // only character.
  const std::size_t last_slash = request_path.rfind('/');
  if (last_slash == 0)
    return "/";
  return std::string(request_path.substr(0, last_slash));
}

bool DomainMatches(std::string_view host, std::string_view cookie_domain) {
  // RFC 6265 §5.1.3: host equals cookie domain, or host is a suffix that
  // begins with a '.' before the cookie domain.
  if (EqualsIgnoreCase(host, cookie_domain))
    return true;
  if (host.size() > cookie_domain.size() + 1 && host[host.size() - cookie_domain.size() - 1] == '.'
      && EqualsIgnoreCase(host.substr(host.size() - cookie_domain.size()), cookie_domain)) {
    return true;
  }
  return false;
}

bool PathMatches(std::string_view request_path, std::string_view cookie_path) {
  // RFC 6265 §5.1.4: request path is the cookie path, or starts with it and
  // the next character is '/', or cookie path is the prefix and ends in '/'.
  if (request_path == cookie_path)
    return true;
  if (request_path.size() > cookie_path.size() && request_path.compare(0, cookie_path.size(), cookie_path) == 0) {
    return cookie_path.back() == '/' || request_path[cookie_path.size()] == '/';
  }
  return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Cookie
// ---------------------------------------------------------------------------
std::optional<Cookie> Cookie::Parse(std::string_view set_cookie_value, const Url &request_url) {
  if (set_cookie_value.empty())
    return std::nullopt;

  // Split name=value (up to first ';') from the attribute list.
  const std::size_t semi = set_cookie_value.find(';');
  const std::string_view nv = semi == std::string_view::npos ? set_cookie_value : set_cookie_value.substr(0, semi);
  const std::size_t eq = nv.find('=');
  if (eq == std::string_view::npos || eq == 0)
    return std::nullopt; // no name, or no '='.

  Cookie cookie;
  cookie.name = ToLower(std::string(nv.substr(0, eq)));
  cookie.value = std::string(nv.substr(eq + 1));
  TrimWhitespace(&cookie.value);
  // RFC 6265 forbids control chars / ';' in names.
  for (unsigned char c : cookie.name) {
    if (c <= 0x20 || c == ';' || c == ',' || c == '"' || c == '\\')
      return std::nullopt;
  }

  const std::string default_domain = ToLower(std::string(request_url.host()));
  if (default_domain.empty())
    return std::nullopt;
  cookie.domain = default_domain;
  cookie.path = DefaultPath(request_url.path());
  const Time now = Time::Now();

  // Attribute list.
  std::size_t pos = semi == std::string_view::npos ? set_cookie_value.size() : semi + 1;
  while (pos < set_cookie_value.size()) {
    while (pos < set_cookie_value.size()
           && (set_cookie_value[pos] == ';' || set_cookie_value[pos] == ' ' || set_cookie_value[pos] == '\t'))
      ++pos;
    const std::size_t attr_end = set_cookie_value.find(';', pos);
    const std::string_view attr = attr_end == std::string_view::npos ? set_cookie_value.substr(pos)
                                                                     : set_cookie_value.substr(pos, attr_end - pos);
    pos = attr_end == std::string_view::npos ? set_cookie_value.size() : attr_end + 1;

    const std::size_t attr_eq = attr.find('=');
    const std::string_view key = attr_eq == std::string_view::npos ? attr : attr.substr(0, attr_eq);
    std::string attr_value = attr_eq == std::string_view::npos ? std::string() : std::string(attr.substr(attr_eq + 1));
    TrimWhitespace(&attr_value);

    if (EqualsIgnoreCase(key, "Domain")) {
      if (attr_value.empty())
        continue;
      std::string domain = ToLower(attr_value);
      // Strip a single leading '.'.
      if (domain.size() > 1 && domain.front() == '.')
        domain.erase(0, 1);
      // The Domain attribute must be a suffix of the request host
      // (RFC 6265 §5.3.6); otherwise reject the cookie.
      if (!DomainMatches(default_domain, domain))
        return std::nullopt;
      cookie.domain = std::move(domain);
      cookie.host_only = false;
    } else if (EqualsIgnoreCase(key, "Path")) {
      if (!attr_value.empty()) {
        cookie.path = attr_value.front() == '/' ? std::move(attr_value) : DefaultPath(request_url.path());
      }
    } else if (EqualsIgnoreCase(key, "Max-Age")) {
      char *end = nullptr;
      const long long seconds = std::strtoll(attr_value.c_str(), &end, 10);
      if (end != attr_value.c_str()) {
        if (seconds <= 0) {
          cookie.expires = Time::UnixEpoch(); // already expired
        } else {
          cookie.expires = Time::FromUnixSeconds(now.ToUnixSeconds() + seconds);
        }
      }
    } else if (EqualsIgnoreCase(key, "Expires")) {
      // Max-Age wins over Expires (RFC 6265 §5.3); only apply Expires when no
      // Max-Age was seen.
      if (!cookie.expires.has_value()) {
        if (const auto parsed = ParseHttpDate(attr_value))
          cookie.expires = Time::FromUnixSeconds(*parsed);
      }
    } else if (EqualsIgnoreCase(key, "Secure")) {
      cookie.secure = true;
    } else if (EqualsIgnoreCase(key, "HttpOnly")) {
      cookie.http_only = true;
    }
    // SameSite / SameParty / Priority / unknown: ignored.
  }

  // A cookie with an explicit Domain that is host-only-requested is fine; a
  // cookie whose path is invalid falls back to default.
  return cookie;
}

std::string Cookie::ToCookiePair() const {
  std::string out = name;
  out += '=';
  out += value;
  return out;
}

bool Cookie::Matches(const Url &request_url, Time now) const {
  if (IsExpired(now))
    return false;
  if (secure && !EqualsIgnoreCase(request_url.scheme(), "https"))
    return false;
  // Host-only cookies (no Domain attribute) are only sent to the exact host.
  if (host_only) {
    if (!EqualsIgnoreCase(request_url.host(), domain))
      return false;
  } else if (!DomainMatches(request_url.host(), domain)) {
    return false;
  }
  return PathMatches(request_url.path(), path);
}

bool Cookie::IsExpired(Time now) const {
  return expires.has_value() && *expires <= now;
}

// ---------------------------------------------------------------------------
// CookieJar
// ---------------------------------------------------------------------------
void CookieJar::SetCookie(const Cookie &cookie) {
  if (cookie.name.empty())
    return;
  if (cookie.IsExpired(Time::Now()))
    return;
  for (auto &existing : cookies_) {
    if (existing.name == cookie.name && existing.domain == cookie.domain && existing.path == cookie.path) {
      existing = cookie;
      return;
    }
  }
  cookies_.push_back(cookie);
}

std::string CookieJar::GetCookieHeader(const Url &request_url) {
  const Time now = Time::Now();
  std::string header;
  for (const auto &cookie : cookies_) {
    if (cookie.Matches(request_url, now)) {
      if (!header.empty())
        header += "; ";
      header += cookie.ToCookiePair();
    }
  }
  return header;
}

void CookieJar::Clear() {
  cookies_.clear();
}

} // namespace nei::net::http
