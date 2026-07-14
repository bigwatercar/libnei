cmake_minimum_required(VERSION 3.23)

project(
  nei
  VERSION 0.1.0
  DESCRIPTION
    "libNei: A modern, cross-platform C/C++ foundation library focusing on performance, safety, and ease of use."
  LANGUAGES C CXX
)

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

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
option(BUILD_SHARED_LIBS "Build shared libraries" ON)

if(NEI_BUILD_TESTS)
  include(CTest)
endif()

# Set default build type to Debug if no configuration is specified
# (single-config generators)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  message(WARNING "No build type specified, defaulting to Debug")
  set(CMAKE_BUILD_TYPE
      Debug
      CACHE STRING "Build type" FORCE
  )
endif()

if(NEI_ENABLE_WARNINGS)
  if(MSVC)
    add_compile_options(/W4 /permissive-)
  else()
    add_compile_options(-Wall -Wextra -Wpedantic)
  endif()
  if(NEI_TREAT_WARNINGS_AS_ERRORS)
    if(MSVC)
      add_compile_options(/WX)
    else()
      add_compile_options(-Werror)
    endif()
  endif()
endif()

# ============================================================================
# nei library target (single DLL, C + optional C++ components)
# ============================================================================
add_library(nei)
add_library(nei::nei ALIAS nei)

# Platform definitions
if(WIN32)
  target_compile_definitions(nei PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(nei PRIVATE /utf-8)
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
    set_target_properties(
      nei
      PROPERTIES C_VISIBILITY_PRESET hidden
                 CXX_VISIBILITY_PRESET hidden
                 VISIBILITY_INLINES_HIDDEN YES
                 POSITION_INDEPENDENT_CODE ON
    )
  endif()
else()
  target_compile_definitions(nei PUBLIC NEI_STATIC)
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

# ============================================================================
# Third-party dependencies (c-ares, ...)
# ============================================================================
add_subdirectory(3rdparty)

# ============================================================================

add_subdirectory(modules/nei)

if(NEI_BUILD_NEIXX)
  add_subdirectory(modules/neixx)
endif()

if(NEI_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(NEI_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

if(NEI_BUILD_DEMOS)
  add_subdirectory(demo)
endif()

install(
  TARGETS nei
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
