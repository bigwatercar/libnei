// =============================================================================
// redirect_handler.h — HTTP redirect (3xx) decision logic
// =============================================================================
//
// Pure logic for RFC 9110 redirect handling: given a response and the URL of
// the request that produced it, decide whether/how to follow the redirect.
// The caller performs the follow (this component does no I/O or URL fetching).
//
//   - 301 Moved Permanently / 302 Found / 303 See Other: the request method
//     is rewritten to GET (except HEAD, which stays HEAD) and the body is
//     dropped (RFC 9110 §15.4).
//   - 307 Temporary Redirect / 308 Permanent Redirect: method and body are
//     preserved.
//   - Location may be a relative reference (RFC 3986 §5), resolved against
//     the original request URL.
//
// Hop budgets and loop protection are the caller's responsibility; this
// component only reports whether one more hop is allowed via |remaining_hops|.

#ifndef NEIXX_NET_HTTP_REDIRECT_HANDLER_H_
#define NEIXX_NET_HTTP_REDIRECT_HANDLER_H_

#include <optional>
#include <string>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_response.h>
#include <neixx/url/url.h>

namespace nei::net::http {

// Result of evaluating one redirect response.
struct NEI_API RedirectDecision {
  // True when the response is a redirect and |remaining_hops| allowed it.
  bool follow = false;
  // Absolute URL for the next request (Location resolved against the
  // original URL).
  Url target;
  // Method for the next request (rewritten to GET for 301/302/303 unless it
  // already was GET/HEAD).
  HttpMethod method = HttpMethod::kUnknown;
  // True when |method| differs from the original request method (the caller
  // must drop the request body / switch to GET semantics).
  bool method_changed = false;
  // Original redirect status code (301/302/303/307/308), 0 when not a
  // redirect.
  int status_code = 0;
};

// Evaluates |response| for a redirect.  |original_url| is the URL of the
// request that produced |response|; |original_method| its method.
// |remaining_hops| is how many redirect hops the caller still allows (<= 0
// means "no more hops" → follow=false).  Returns nullopt when the response is
// not a redirect or has no usable Location.
NEI_API std::optional<RedirectDecision>
ComputeRedirect(const HttpResponse &response, const Url &original_url, HttpMethod original_method, int remaining_hops);

} // namespace nei::net::http

#endif // NEIXX_NET_HTTP_REDIRECT_HANDLER_H_
