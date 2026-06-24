target_sources(
  neixx
  PRIVATE include/neixx/common/at_exit.h
          include/neixx/common/location.h
          include/neixx/common/no_destructor.h
          include/neixx/common/path_service.h
          include/neixx/common/sequence_checker.h
          include/neixx/common/singleton.h
          include/neixx/common/thread_checker.h
          include/neixx/common/time.h
          include/neixx/common/time_source.h
          src/at_exit.cpp
          src/location.cpp
          src/path_service.cpp
          src/time.cpp
          src/time_source.cpp
)

target_include_directories(
  neixx PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(WIN32)
  target_sources(neixx PRIVATE src/path_service_win.cpp)
elseif(APPLE)
  target_sources(neixx PRIVATE src/path_service_mac.cpp)
elseif(UNIX)
  target_sources(neixx PRIVATE src/path_service_linux.cpp)
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
