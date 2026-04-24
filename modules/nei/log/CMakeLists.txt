target_sources(
    nei
    PRIVATE
        src/log_config.c
        src/log_sink.c
        src/log_api.c
        src/log_thread_id.c
        src/log_serialize.c
        src/log_format.c
        src/log_runtime.c
)

target_include_directories(
    nei
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_definitions(
    nei
    PRIVATE
        NEI_LOG_RING_SLOTS=${NEI_LOG_RING_SLOTS}
)

install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
