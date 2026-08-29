target_sources(
  nei
  PRIVATE ${PROJECT_SOURCE_DIR}/include/nei/log/log.h
  PRIVATE log_api.c
          log_config.c
          log_format.c
          log_internal.h
          log_runtime.c
          log_serialize.c
          log_sink.c
          log_thread_id.c
)

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_definitions(nei PRIVATE NEI_LOG_RING_SLOTS=${NEI_LOG_RING_SLOTS})

if(WIN32)
  target_link_libraries(nei PRIVATE Dbghelp)
endif()

