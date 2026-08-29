target_sources(
  nei
  PRIVATE ${PROJECT_SOURCE_DIR}/include/neixx/common/at_exit.h
          ${PROJECT_SOURCE_DIR}/include/neixx/common/location.h
          ${PROJECT_SOURCE_DIR}/include/neixx/common/no_destructor.h
          ${PROJECT_SOURCE_DIR}/include/neixx/common/path_service.h
          ${PROJECT_SOURCE_DIR}/include/neixx/common/platform_handle.h
          ${PROJECT_SOURCE_DIR}/include/neixx/common/singleton.h
          ${PROJECT_SOURCE_DIR}/include/neixx/common/strong_alias.h
          ${PROJECT_SOURCE_DIR}/include/neixx/common/time.h
          ${PROJECT_SOURCE_DIR}/include/neixx/common/time_source.h
          at_exit.cpp
          location.cpp
          path_service.cpp
          time.cpp
          time_source.cpp
)

if(WIN32)
  target_sources(
    nei PRIVATE ${PROJECT_SOURCE_DIR}/include/neixx/common/scoped_handle.h path_service_win.cpp
                platform_handle_win.cpp
  )
elseif(APPLE)
  target_sources(
    nei PRIVATE ${PROJECT_SOURCE_DIR}/include/neixx/common/scoped_fd.h path_service_mac.cpp
                platform_handle_posix.cpp
  )
elseif(UNIX)
  target_sources(
    nei PRIVATE ${PROJECT_SOURCE_DIR}/include/neixx/common/scoped_fd.h path_service_linux.cpp
                platform_handle_posix.cpp
  )
endif()

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

