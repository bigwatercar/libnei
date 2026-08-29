#ifndef NGHTTP2_VENDORED_CONFIG_H_
#define NGHTTP2_VENDORED_CONFIG_H_

/* Minimal config.h for the vendored lib-only nghttp2 build (no generated
   config).  The single platform gap is ssize_t: MSVC's <sys/types.h> does
   not define it, while the public nghttp2.h uses it in several callback
   signatures.  ptrdiff_t is the exact type nghttp2 itself uses for
   nghttp2_ssize. */
#if defined(_MSC_VER) && !defined(ssize_t)
#include <stddef.h>
typedef ptrdiff_t ssize_t;
#endif

#endif /* NGHTTP2_VENDORED_CONFIG_H_ */
