set(NEIXX_PROCESS_SOURCES
    ${PROJECT_SOURCE_DIR}/include/neixx/process/child_process.h
    ${PROJECT_SOURCE_DIR}/include/neixx/process/process_service.h
    ${PROJECT_SOURCE_DIR}/include/neixx/process/process_util.h
    child_process.cpp
    child_process_stream_proxy.h
    child_process_stream_proxy.cpp
)

if(WIN32)
  list(
    APPEND
    NEIXX_PROCESS_SOURCES
    process_service.cpp
    child_process_win.cpp
    process_util_win.cpp
  )
else()
  list(
    APPEND
    NEIXX_PROCESS_SOURCES
    process_service.cpp
    child_process_posix.cpp
    process_util_posix.cpp
  )
endif()

target_sources(nei PRIVATE ${NEIXX_PROCESS_SOURCES})

target_include_directories(
  nei
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

if(WIN32)
  target_link_libraries(nei PRIVATE shell32)
endif()

