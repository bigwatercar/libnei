// =============================================================================
// redirect_handler.cpp — HTTP redirect (3xx) decision logic
// =============================================================================

#include <neixx/net/http/redirect_handler.h>

namespace nei::net::http {

namespace {

bool IsRedirectStatus(int code) {
  return code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
}

} // namespace

std::optional<RedirectDecision>
ComputeRedirect(const HttpResponse &response, const Url &original_url, HttpMethod original_method, int remaining_hops) {
  if (remaining_hops <= 0)
    return std::nullopt;
  const int code = response.status.raw_code();
  if (!IsRedirectStatus(code))
    return std::nullopt;

  const std::string location = std::string(response.GetHeaderValue("Location"));
  if (location.empty())
    return std::nullopt;

  RedirectDecision decision;
  decision.status_code = code;
  // Location may be a relative reference; resolve against the original URL.
  // The target must be an absolute http(s) URL.
  const Url resolved = original_url.Resolve(location);
  const std::string scheme = std::string(resolved.scheme());
  if (!resolved.is_valid() || (scheme != "http" && scheme != "https") || resolved.host().empty())
    return std::nullopt;
  decision.target = resolved;

  // RFC 9110 §15.4: 301/302/303 rewrite to GET (HEAD stays HEAD); 307/308
  // preserve the method and body.
  if (code == 301 || code == 302 || code == 303) {
    if (original_method != HttpMethod::kGet && original_method != HttpMethod::kHead) {
      decision.method = HttpMethod::kGet;
      decision.method_changed = true;
    } else {
      decision.method = original_method;
    }
  } else {
    decision.method = original_method;
  }

  decision.follow = true;
  return decision;
}

} // namespace nei::net::http
