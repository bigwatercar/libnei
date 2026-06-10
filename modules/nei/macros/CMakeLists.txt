target_sources(
  nei
  PRIVATE include/nei/macros/compiler_specific.h include/nei/macros/namespace.h
          include/nei/macros/nei_export.h include/nei/macros/typedefs.h
)

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
