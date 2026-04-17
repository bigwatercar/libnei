cmake_minimum_required(VERSION 3.23)

project(
    nei
    VERSION 0.1.0
    DESCRIPTION "A modular C/C++ library template"
    LANGUAGES C CXX
)

# Export compile_commands.json for clangd / IDE tooling (no code changes required).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(NEI_BUILD_TESTS "Build nei tests" ON)
option(NEI_ENABLE_WARNINGS "Enable strict warnings" ON)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

# Compatible with both single-config and multi-config generators.
set(NEI_SUPPORTED_CONFIGS Debug Release RelWithDebInfo MinSizeRel)
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${NEI_SUPPORTED_CONFIGS}" CACHE STRING "Available build configurations" FORCE)
else()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${NEI_SUPPORTED_CONFIGS})
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    endif()
endif()

if(NEI_ENABLE_WARNINGS)
    if(MSVC)
        add_compile_options(/W4 /permissive-)
    else()
        add_compile_options(-Wall -Wextra -Wpedantic)
    endif()
endif()

add_library(nei)
add_library(nei::nei ALIAS nei)

target_compile_features(nei PUBLIC cxx_std_17)
if(BUILD_SHARED_LIBS)
    target_compile_definitions(nei PRIVATE NEI_EXPORTS)
    if(NOT WIN32)
        set_target_properties(
            nei
            PROPERTIES
                C_VISIBILITY_PRESET hidden
                CXX_VISIBILITY_PRESET hidden
                VISIBILITY_INLINES_HIDDEN YES
        )
    endif()
else()
    target_compile_definitions(nei PUBLIC NEI_STATIC)
endif()

add_subdirectory(modules/macros)
add_subdirectory(modules/nei/log)
add_subdirectory(modules/nei/utils)
add_subdirectory(modules/nei/xdr)
add_subdirectory(modules/nei/core)
add_subdirectory(modules/neixx/task)

if(NEI_BUILD_TESTS)
    add_subdirectory(tests)
endif()

install(
    TARGETS nei
    EXPORT neiTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(
    EXPORT neiTargets
    FILE neiTargets.cmake
    NAMESPACE nei::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nei
)

configure_package_config_file(
    cmake/neiConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/neiConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nei
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/neiConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/neiConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/neiConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nei
)
