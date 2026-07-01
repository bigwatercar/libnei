set(NEIXX_PROCESS_SOURCES
  include/neixx/process/child_process.h
  include/neixx/process/process_service.h
  include/neixx/process/process_util.h
  src/child_process.cpp
  src/child_process_stream_proxy.h
  src/child_process_stream_proxy.cpp
)

if(WIN32)
  list(APPEND NEIXX_PROCESS_SOURCES
    src/process_service.cpp
    src/child_process_win.cpp
    src/process_util_win.cpp
  )
else()
  list(APPEND NEIXX_PROCESS_SOURCES
    src/process_service.cpp
    src/child_process_posix.cpp
    src/process_util_posix.cpp
  )
endif()

target_sources(nei PRIVATE ${NEIXX_PROCESS_SOURCES})

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WIN32)
  target_link_libraries(nei PRIVATE shell32)
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
