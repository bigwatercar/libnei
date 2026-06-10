target_sources(
  nei
  PRIVATE include/nei/core/endian.h include/nei/core/float_ctrl.h
  PRIVATE src/endian.c src/float_ctrl.c
)

if(WIN32)
  target_sources(nei PRIVATE src/endian_win.c)
else()
  target_sources(nei PRIVATE src/endian_posix.c)
endif()

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
