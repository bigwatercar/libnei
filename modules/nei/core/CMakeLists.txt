target_sources(
  nei
  PRIVATE include/nei/core/endian.h include/nei/core/float_ctrl.h
          include/nei/core/encoding.h include/nei/core/file_util.h
          include/nei/core/path_util.h include/nei/core/time.h
  PRIVATE src/endian.c src/float_ctrl.c src/encoding.c src/file_util.c
          src/path_util.c src/time.c
)

if(WIN32)
  target_sources(nei PRIVATE src/endian_win.c src/file_util_win.c src/path_util_win.c)
else()
  target_sources(nei PRIVATE src/endian_posix.c src/file_util_posix.c src/path_util_posix.c)
endif()

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
