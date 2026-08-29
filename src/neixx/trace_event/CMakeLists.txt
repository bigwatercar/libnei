target_sources(
  nei PRIVATE ${PROJECT_SOURCE_DIR}/include/neixx/trace_event/trace_event.h
              ${PROJECT_SOURCE_DIR}/include/neixx/trace_event/trace_log.h trace_log.cpp
)

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

