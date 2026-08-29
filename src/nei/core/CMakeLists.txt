target_sources(
  nei
  PRIVATE ${PROJECT_SOURCE_DIR}/include/nei/core/endian.h
          ${PROJECT_SOURCE_DIR}/include/nei/core/float_ctrl.h
          ${PROJECT_SOURCE_DIR}/include/nei/core/encoding.h
          ${PROJECT_SOURCE_DIR}/include/nei/core/file_util.h
          ${PROJECT_SOURCE_DIR}/include/nei/core/path_util.h
          ${PROJECT_SOURCE_DIR}/include/nei/core/time.h
          ${PROJECT_SOURCE_DIR}/include/nei/core/random.h
          ${PROJECT_SOURCE_DIR}/include/nei/core/version.h
  PRIVATE endian.c
          file_util.c
          float_ctrl.c
          encoding.c
          path_util.c
          random.c
          version.c
)

if(WIN32)
  target_sources(
    nei PRIVATE endian_win.c file_util_win.c path_util_win.c
                time_win.c
  )
else()
  target_sources(
    nei PRIVATE endian_posix.c file_util_posix.c path_util_posix.c
                time_posix.c
  )
endif()

target_include_directories(
  nei
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

