target_sources(
  nei
  PRIVATE include/nei/build/compiler_specific.h
          include/nei/build/nei_export.h
          include/nei/build/nei_global.h
          nei_global.c
)

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
