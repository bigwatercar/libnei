set(NEIXX_STORAGE_SOURCES
    ${PROJECT_SOURCE_DIR}/include/neixx/storage/storage_monitor.h
    storage_monitor_impl.h
    storage_monitor.cpp
)

if(WIN32)
  list(APPEND NEIXX_STORAGE_SOURCES storage_monitor_win.cpp)
else()
  list(APPEND NEIXX_STORAGE_SOURCES storage_monitor_posix.cpp)
endif()

target_sources(nei PRIVATE ${NEIXX_STORAGE_SOURCES})

target_include_directories(
  nei
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
