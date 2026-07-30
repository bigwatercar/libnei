# =============================================================================
# Third-party dependencies for libnei
# =============================================================================

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

# mbedTLS vendored C code triggers numerous MSVC warnings (C4244, C4200,
# C4295, C4701, etc.) that are harmless but noisy.  Suppress all compiler
# warnings for third-party targets — we don't own this code.
set(_mbedtls_targets mbedtls mbedx509 mbedcrypto p256m everest)
foreach(tgt IN LISTS _mbedtls_targets)
  if(TARGET ${tgt})
    target_compile_options(${tgt} PRIVATE
      $<$<C_COMPILER_ID:MSVC>:/w /WX->)
  endif()
endforeach()
unset(_mbedtls_targets)

# c-ares also triggers C4267 warnings.  Suppress likewise.
if(TARGET c-ares)
  target_compile_options(c-ares PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/wd4267 /WX->)
endif()
