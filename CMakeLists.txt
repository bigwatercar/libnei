cmake_minimum_required(VERSION 3.23)

project(
  nei
  VERSION 0.9.0
  DESCRIPTION
    "libNei: A modern, cross-platform C/C++ foundation library focusing on performance, safety, and ease of use."
  LANGUAGES C CXX
)

# Set default build type to Debug if no configuration is specified
# (single-config generators)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  message(WARNING "No build type specified, defaulting to Debug")
  set(CMAKE_BUILD_TYPE
      Debug
      CACHE STRING "Build type" FORCE
  )
endif()

# Single source of truth for the library version.  All downstream consumers
# (generated version header, SONAME, package version file, release tags) derive
# from the project() VERSION above -- bump ONLY that line when releasing.
#
# SOVERSION policy: during the 0.x pre-release phase the ABI is explicitly
# unstable, so SOVERSION tracks major.minor (0.9) and every minor release
# requires relinking.  At 1.0, switch SOVERSION to ${PROJECT_VERSION_MAJOR} so
# that only major releases break the ABI.
set(NEI_VERSION "${PROJECT_VERSION}")

# Export compile_commands.json for clangd / IDE tooling (no code changes
# required).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# C/C++ standard defaults (overridable via -DCMAKE_C_STANDARD=... etc.)
if(NOT CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 11)
endif()
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Hide all symbols by default.  Public API symbols are explicitly exported via
# NEI_API (__attribute__((visibility("default")))). This prevents third-party
# static libraries (mbedtls, c-ares) from leaking their symbols into libnei.so's
# public ABI.  These visibility settings are ELF-only (GNU/Clang) and are no-ops
# on Windows/MSVC, where DLL exports are governed by __declspec(dllexport) via
# NEI_API, so they can be set unconditionally.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN YES)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(neiDetectCompiler)

option(NEI_BUILD_TESTS "Build nei tests" ON)
option(NEI_BUILD_BENCHMARKS "Build nei benchmarks" ON)
option(NEI_BUILD_DEMOS "Build nei demos" ON)
option(NEI_ENABLE_WARNINGS "Enable strict warnings" ON)
option(NEI_TREAT_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(NEI_ENABLE_LONG_BENCH_TESTS
       "Enable long-running benchmark tests in CTest" OFF
)
option(NEI_BUILD_NEIXX "Build neixx C++ components" ON)
option(NEI_ENABLE_TRACE_EVENTS "Enable trace event instrumentation(C++ only)"
       ON
)
option(
  NEI_ENABLE_PARALLEL_DIAGNOSTICS
  "Enable per-task parallel scheduler diagnostics (g_parallel_* counters + FlushForTesting posted/completed accounting). Disable for ~8-22% higher PostTask throughput."
  OFF
)
option(
  NEI_ENABLE_ALLOCATOR_DIAGNOSTICS
  "Enable per-allocation SmallObjectAllocator diagnostics (pooled/direct/in_use_by_class atomic counters). Disable to reduce hot-path atomic RMW overhead."
  OFF
)
option(BUILD_SHARED_LIBS "Build shared libraries" ON)

# ============================================================================
# Third-party dependencies (c-ares, ...)
# ============================================================================
message(STATUS "Configuring third-party dependencies...")
add_subdirectory(external)
message(STATUS "Third-party dependencies configured.")

if(NEI_BUILD_TESTS)
  include(CTest)
endif()

if(NEI_ENABLE_WARNINGS)
  if(NEI_MSVC_FLAVOR)
    add_compile_options(/W4 /permissive-)
  elseif(NEI_GNU_FLAVOR)
    add_compile_options(-Wall -Wextra -Wpedantic)
  endif()
  if(NEI_TREAT_WARNINGS_AS_ERRORS)
    if(NEI_MSVC_FLAVOR)
      add_compile_options(/WX)
    elseif(NEI_GNU_FLAVOR)
      add_compile_options(-Werror)
    endif()
  endif()
endif()

# Treat source and execution character sets as UTF-8 on MSVC. Without this flag,
# MSVC encodes \uXXXX escapes in narrow string literals using the system ANSI
# code page (e.g. CP936), producing C4819 warnings and incorrect byte sequences
# at runtime.
if(NEI_MSVC_FLAVOR)
  add_compile_options(/utf-8)
endif()

# ============================================================================
# nei library target (single DLL, C + optional C++ components)
# ============================================================================
add_library(nei)
add_library(nei::nei ALIAS nei)

# Versioned shared library (Linux: libnei.so.0.9.0 + libnei.so.0.9 soname
# symlink; Windows DLLs carry no SONAME so these are ELF-only no-ops).
set_target_properties(
  nei PROPERTIES VERSION ${PROJECT_VERSION}
                 SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
)

# Platform definitions
if(WIN32)
  target_compile_definitions(nei PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
  if(NEI_MSVC_FLAVOR)
    # /Zc:__cplusplus — MSVC reports 199711L by default even under /std:c++17.
    # This flag makes __cplusplus reflect the actual standard version (e.g.
    # 201703L for C++17).  Applied at the top-level nei target so that C++
    # wrapper sources in the pure-C nei module also get correct
    # feature-detection macros.
    target_compile_options(nei PUBLIC /Zc:__cplusplus)
  endif()
endif()

# Shared / static library configuration
if(BUILD_SHARED_LIBS)
  target_compile_definitions(nei PRIVATE NEI_EXPORTS)
  if(NOT WIN32)
    # Hide all symbols originating from static libraries (mbedtls, c-ares, etc.)
    # so they don't leak into the public ABI of libnei.so.
    target_link_options(nei PRIVATE -Wl,--exclude-libs,ALL)
  endif()
else()
  target_compile_definitions(nei PUBLIC NEI_STATIC)
endif()

# Parallel scheduler diagnostics switch.  Public so consumers compiling against
# the library headers see the same NEI_PARALLEL_DIAGNOSTICS value as the library
# itself (the macro must be identical across all TUs).
if(NEI_ENABLE_PARALLEL_DIAGNOSTICS)
  target_compile_definitions(nei PUBLIC NEI_PARALLEL_DIAGNOSTICS=1)
else()
  target_compile_definitions(nei PUBLIC NEI_PARALLEL_DIAGNOSTICS=0)
endif()

# SmallObjectAllocator per-allocation diagnostic counters. Turn off to eliminate
# ~2-3 relaxed atomic RMW per allocation on the hot path.
if(NEI_ENABLE_ALLOCATOR_DIAGNOSTICS)
  target_compile_definitions(nei PUBLIC NEI_ALLOCATOR_DIAGNOSTICS=1)
else()
  target_compile_definitions(nei PUBLIC NEI_ALLOCATOR_DIAGNOSTICS=0)
endif()

# Non-Windows dependencies
if(NOT WIN32)
  # _POSIX_C_SOURCE=199309L is required for clock_gettime / struct timespec when
  # compiling in strict C11 mode (otherwise GCC hides these POSIX types).
  target_compile_definitions(nei PUBLIC _POSIX_C_SOURCE=199309L)
  find_package(Threads REQUIRED)
  target_link_libraries(
    nei
    PUBLIC Threads::Threads
    PRIVATE m
  )
endif()

add_subdirectory(src/nei)

if(NEI_BUILD_NEIXX)
  add_subdirectory(src/neixx)
endif()

if(NEI_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(NEI_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

if(NEI_BUILD_DEMOS)
  add_subdirectory(examples)
endif()

# Static builds need the third-party libs in the export set because they are
# linked into libnei.a and CMake requires all transitive dependencies. Shared
# builds only export nei — mbedtls/c-ares are PRIVATE and do not leak into the
# public ABI (visibility hidden + --exclude-libs,ALL).
if(BUILD_SHARED_LIBS)
  set(_nei_install_targets nei)
else()
  set(_nei_install_targets
      nei
      # external
      c-ares
      mbedtls
      mbedx509
      mbedcrypto
      everest
      p256m
  )
endif()

install(
  TARGETS ${_nei_install_targets}
  EXPORT neiTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/$<CONFIG>
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/$<CONFIG>
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/$<CONFIG>
)

install(
  EXPORT neiTargets
  FILE neiTargets.cmake
  NAMESPACE nei::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nei
)

# Pitchfork Layout: single public include root.  All public headers live under
# include/ mirroring the namespace structure (include/nei/...,
# include/neixx/...).
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_package_config_file(
  cmake/neiConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/neiConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nei
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/neiConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/neiConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/neiConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nei
)
