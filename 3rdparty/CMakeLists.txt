# =============================================================================
# Third-party dependencies for libnei
# =============================================================================
#
# All vendored third-party libraries are INTERNAL-ONLY: they are built and
# linked PRIVATEly into libnei but are never installed into the public install
# tree (which must only carry libnei itself and its own headers).  Each
# library's install rules are gated by the switches below, plus a vendored
# guard on mbedtls' library install.

# Generic skip for libraries that honour SKIP_INSTALL_ALL (zlib, ...).
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
# c-ares: install target switch.
set(CARES_INSTALL OFF CACHE BOOL "" FORCE)
# mbedtls: headers + package config/target export switches.
set(INSTALL_MBEDTLS_HEADERS OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)

# ---------------------------------------------------------------------------
# c-ares (MIT) — asynchronous DNS resolver
# ---------------------------------------------------------------------------
# Build as a static library with all extras disabled.  c-ares is linked
# PRIVATEly into nei so it never leaks into the public ABI.
set(CARES_STATIC ON CACHE BOOL "" FORCE)
set(CARES_STATIC_PIC ON CACHE BOOL "" FORCE)
set(CARES_SHARED OFF CACHE BOOL "" FORCE)
set(CARES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CARES_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(CARES_INSTALL OFF CACHE BOOL "" FORCE)

add_subdirectory(c-ares)

# The upstream CMake target is named "c-ares" (with hyphen).  Create an
# alias so that we can refer to it with a consistent name.
if(NOT TARGET cares)
  add_library(cares ALIAS c-ares)
endif()

# c-ares defaults to C90 which is incompatible with modern Windows SDK
# headers (they use // comments).  Force at least C99.
set_target_properties(c-ares PROPERTIES C_STANDARD 99)
set_target_properties(c-ares PROPERTIES C_STANDARD_REQUIRED ON)

# Suppress MSVC warnings from vendored C code.  c-ares is C90-era code
# that triggers /W4 warnings (C4456 shadowing, C4267 size_t truncation,
# C4706 assignment-in-conditional, etc.).  These are upstream-accepted
# patterns; we silence them at the target level rather than patching.
if(COMPILER_IS_MSVC)
  target_compile_options(c-ares PRIVATE /W3 /wd4456 /wd4267 /wd4706)
endif()

# On Linux with CMAKE_BUILD_TYPE=Release, CMake's CheckSymbolExists can
# lose the C compiler context inside try_compile, causing false negatives
# for standard POSIX socket symbols.  Pre-set the known results to avoid
# the broken checks.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(HAVE_SEND 1 CACHE INTERNAL "send() is POSIX")
  set(HAVE_SENDTO 1 CACHE INTERNAL "sendto() is POSIX")
  set(HAVE_RECV 1 CACHE INTERNAL "recv() is POSIX")
  set(HAVE_RECVFROM 1 CACHE INTERNAL "recvfrom() is POSIX")
  set(HAVE_SOCKET 1 CACHE INTERNAL "socket() is POSIX")
  set(HAVE_SETSOCKOPT 1 CACHE INTERNAL "setsockopt() is POSIX")
  set(HAVE_CONNECT 1 CACHE INTERNAL "connect() is POSIX")
  set(HAVE_FCNTL 1 CACHE INTERNAL "fcntl() is POSIX")
  set(HAVE_IOCTL 1 CACHE INTERNAL "ioctl() is POSIX")
  set(HAVE_POLL 1 CACHE INTERNAL "poll() is POSIX")
  set(HAVE_PIPE 1 CACHE INTERNAL "pipe() is POSIX")
  set(HAVE_PIPE2 1 CACHE INTERNAL "pipe2() is POSIX")
  set(HAVE_WRITEV 1 CACHE INTERNAL "writev() is POSIX")
  set(HAVE_GETTIMEOFDAY 1 CACHE INTERNAL "gettimeofday() is POSIX")
endif()

# ---------------------------------------------------------------------------
# mbedTLS (Apache-2.0) — TLS/SSL library (v3.6.3 LTS)
# ---------------------------------------------------------------------------
# Build as a static library regardless of the top-level BUILD_SHARED_LIBS.
# mbedTLS produces three library targets: mbedtls → mbedx509 → mbedcrypto.
# Linking mbedtls is sufficient to pull in all three via CMake deps.
set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_AS_SUBDIRECTORY ON CACHE BOOL "" FORCE)

add_subdirectory(mbedtls)

set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})

# mbedTLS vendored C code triggers MSVC warnings that our project promotes
# to errors (/WX).  Suppress both /WX and aggressive /W4 diagnostics
# per-target — covers main libs and bundled third-party crypto (p256-m,
# everest).  /W3 is the MSVC default; third-party code should not be
# subjected to our project's /W4 strictness.
set(_mbedtls_targets mbedtls mbedx509 mbedcrypto p256m everest)
foreach(tgt IN LISTS _mbedtls_targets)
  if(TARGET ${tgt} AND COMPILER_IS_MSVC)
    target_compile_options(${tgt} PRIVATE /WX- /W3)
  endif()
endforeach()
unset(_mbedtls_targets)

# ---------------------------------------------------------------------------
# llhttp (MIT) — Node.js HTTP/1.1 parser (v9.2.1)
# ---------------------------------------------------------------------------
# Build as a static library.  Pure C99, minimal dependencies.
add_subdirectory(llhttp)

# ---------------------------------------------------------------------------
# nghttp2 (MIT) — HTTP/2 protocol library (v1.70.0, lib-only vendoring)
# ---------------------------------------------------------------------------
# Build as a static library.  See 3rdparty/nghttp2/README.md for the
# vendoring scope.  No generated config.h is required.
add_subdirectory(nghttp2)

# ---------------------------------------------------------------------------
# zlib (zlib license) — gzip/deflate compression (v1.3.1)
# ---------------------------------------------------------------------------
# Build as a static library with examples/tests/install disabled.  Linked
# PRIVATEly into nei so it never leaks into the public ABI.
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_LIBRARIES ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_HEADERS ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_FILES ON CACHE BOOL "" FORCE)
add_subdirectory(zlib)

# Shared-library consumption needs PIC on POSIX.
if(NOT WIN32)
  set_target_properties(zlibstatic PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# Suppress MSVC warnings from vendored C code (zlib predates /W4).
if(COMPILER_IS_MSVC)
  target_compile_options(zlibstatic PRIVATE /W3 /wd4267 /wd4244 /wd4131 /wd4101)
endif()
