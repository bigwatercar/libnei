target_sources(
  nei
  PRIVATE include/nei/log/log.h
  PRIVATE src/log_api.c
          src/log_config.c
          src/log_format.c
          src/log_internal.h
          src/log_runtime.c
          src/log_serialize.c
          src/log_sink.c
          src/log_thread_id.c
)

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_definitions(nei PRIVATE NEI_LOG_RING_SLOTS=${NEI_LOG_RING_SLOTS})

if(WIN32)
  target_link_libraries(nei PRIVATE Dbghelp)
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
