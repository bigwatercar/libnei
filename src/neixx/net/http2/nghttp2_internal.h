#pragma once

#ifndef NEIXX_NET_HTTP2_NGHTTP2_INTERNAL_H_
#define NEIXX_NET_HTTP2_NGHTTP2_INTERNAL_H_

// =============================================================================
// Internal shim — the ONLY place <nghttp2/nghttp2.h> may be included.
//
// The public nghttp2.h uses ssize_t in several callback signatures; MSVC's
// <sys/types.h> does not provide it.  ptrdiff_t is the exact type nghttp2
// itself uses for nghttp2_ssize.  Keep this header out of public headers so
// the ssize_t fixup never leaks into consumers.
// =============================================================================

#if defined(_MSC_VER) && !defined(ssize_t)
#include <stddef.h>
typedef ptrdiff_t ssize_t;
#endif

#include <nghttp2/nghttp2.h>

#endif // NEIXX_NET_HTTP2_NGHTTP2_INTERNAL_H_
