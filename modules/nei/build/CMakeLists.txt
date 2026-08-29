target_sources(
  nei
  PRIVATE include/nei/build/compiler_specific.h
          include/nei/build/nei_export.h
          include/nei/build/nei_global.h
          nei_global.c
)

# Generated version header: the project() VERSION in the root CMakeLists.txt
# is the single source of truth.  The template lives under cmake/ next to the
# package config templates; the configured file lands in the binary dir and is
# exposed through an extra BUILD_INTERFACE include path.
configure_file(
  ${PROJECT_SOURCE_DIR}/cmake/version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/nei/build/version.h
  @ONLY
)

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
             $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/nei/build/version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nei/build
)
