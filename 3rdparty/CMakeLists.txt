# =============================================================================
# Third-party dependencies for libnei
# =============================================================================

# ---------------------------------------------------------------------------
# c-ares (MIT) — asynchronous DNS resolver
# ---------------------------------------------------------------------------
# Build as a static library with all extras disabled.  c-ares is linked
# PRIVATEly into nei so it never leaks into the public ABI.
set(CARES_STATIC ON CACHE BOOL "" FORCE)
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
