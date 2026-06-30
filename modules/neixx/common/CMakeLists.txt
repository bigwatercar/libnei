target_sources(
  nei
  PRIVATE include/neixx/common/at_exit.h
          include/neixx/common/location.h
          include/neixx/common/no_destructor.h
          include/neixx/common/path_service.h
          include/neixx/common/platform_handle.h
          include/neixx/common/singleton.h
          include/neixx/common/time.h
          include/neixx/common/time_source.h
          src/at_exit.cpp
          src/location.cpp
          src/path_service.cpp
          src/time.cpp
          src/time_source.cpp
)

if(WIN32)
  target_sources(nei PRIVATE include/neixx/common/scoped_handle.h
                              src/path_service_win.cpp
                              src/platform_handle_win.cpp)
elseif(APPLE)
  target_sources(nei PRIVATE include/neixx/common/scoped_fd.h
                              src/path_service_mac.cpp
                              src/platform_handle_posix.cpp)
elseif(UNIX)
  target_sources(nei PRIVATE include/neixx/common/scoped_fd.h
                              src/path_service_linux.cpp
                              src/platform_handle_posix.cpp)
endif()

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
