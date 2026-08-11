#pragma once

#ifndef NEIXX_URL_URL_ENCODING_H_
#define NEIXX_URL_URL_ENCODING_H_

#include <string>
#include <string_view>

#include <nei/build/nei_export.h>

namespace nei {

// =============================================================================
// Percent-encoding / decoding (application/x-www-form-urlencoded)
// =============================================================================
//
// UrlEncode:  encodes a raw string for safe inclusion in a URL component.
//             Unreserved characters (RFC 3986 §2.3) are passed through;
//             everything else is percent-encoded.  Space → "%20" (NOT '+').
//
// UrlEncodeQuery: same as UrlEncode but encodes space as '+', matching the
//                 application/x-www-form-urlencoded convention for query
//                 strings and POST bodies.
//
// UrlDecode:  decodes percent-encoded sequences (%XX) back to raw bytes,
//             including '+' → space (application/x-www-form-urlencoded
//             convention).  Invalid sequences (% not followed by two hex
//             digits) are passed through verbatim.

NEI_API std::string UrlEncode(std::string_view raw);
NEI_API std::string UrlEncodeQuery(std::string_view raw);
NEI_API std::string UrlDecode(std::string_view encoded);

} // namespace nei

#endif // NEIXX_URL_URL_ENCODING_H_
