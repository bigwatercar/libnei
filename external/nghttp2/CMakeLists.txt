# =============================================================================
# nghttp2 (MIT) — HTTP/2 protocol library (v1.70.0, lib-only vendoring)
# =============================================================================
#
# Only the core library sources under lib/ are vendored (no apps, tests,
# or Python bindings).  nghttp2's lib does NOT require a generated
# config.h: all HAVE_* guards fall back to portable paths.  We define the
# two clock-related macros per platform so nghttp2_time_now_sec() uses a
# monotonic sub-second clock instead of the wall-clock time() fallback
# (used for PING timing and glitch rate-limiting).

set(NGHTTP2_SOURCES
  lib/nghttp2_pq.c
  lib/nghttp2_map.c
  lib/nghttp2_queue.c
  lib/nghttp2_frame.c
  lib/nghttp2_buf.c
  lib/nghttp2_stream.c
  lib/nghttp2_outbound_item.c
  lib/nghttp2_session.c
  lib/nghttp2_submit.c
  lib/nghttp2_helper.c
  lib/nghttp2_alpn.c
  lib/nghttp2_hd.c
  lib/nghttp2_hd_huffman.c
  lib/nghttp2_hd_huffman_data.c
  lib/nghttp2_version.c
  lib/nghttp2_priority_spec.c
  lib/nghttp2_option.c
  lib/nghttp2_callbacks.c
  lib/nghttp2_mem.c
  lib/nghttp2_http.c
  lib/nghttp2_rcbuf.c
  lib/nghttp2_extpri.c
  lib/nghttp2_ratelim.c
  lib/nghttp2_time.c
  lib/nghttp2_debug.c
  lib/sfparse.c
)

add_library(nghttp2 STATIC ${NGHTTP2_SOURCES})

# The public header layout mirrors the upstream install tree
# (<nghttp2/nghttp2.h>); internal headers live in lib/.
target_include_directories(
  nghttp2
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lib
          ${CMAKE_CURRENT_SOURCE_DIR}/config)

# The lib's internal headers honor HAVE_CONFIG_H; our hand-written
# config/config.h provides the MSVC ssize_t typedef (see that file).
target_compile_definitions(nghttp2 PRIVATE BUILDING_NGHTTP2 HAVE_CONFIG_H=1)

# We always build static: NGHTTP2_STATICLIB is PUBLIC so consumers of the
# public header don't get __declspec(dllimport) on Windows.
target_compile_definitions(nghttp2 PUBLIC NGHTTP2_STATICLIB)

set_target_properties(
  nghttp2
  PROPERTIES C_STANDARD 99
             C_STANDARD_REQUIRED ON
             POSITION_INDEPENDENT_CODE ON)

if(WIN32)
  # GetTickCount64 (kernel32, linked by default on MSVC).
  target_compile_definitions(nghttp2 PRIVATE HAVE_WINDOWS_H=1
                                       HAVE_GETTICKCOUNT64=1)
else()
  # glibc >= 2.17 provides clock_gettime in libc directly; strict C99
  # requires _POSIX_C_SOURCE to expose it.
  target_compile_definitions(nghttp2 PRIVATE _POSIX_C_SOURCE=199309L
                                       HAVE_CLOCK_GETTIME=1
                                       HAVE_DECL_CLOCK_MONOTONIC=1)
endif()

# Vendored C code is not subjected to our project's /W4 + /WX strictness.
if(NEI_MSVC_FLAVOR)
  target_compile_options(nghttp2 PRIVATE /WX- /W3)
endif()
