target_sources(
    nei
    PRIVATE
        src/internal_flag.cpp
        src/task_tracer.cpp
        src/task_runner.cpp
        src/single_thread_task_runner.cpp
        src/thread.cpp
        src/thread_pool.cpp
        src/sequenced_task_runner.cpp
)

target_include_directories(
    nei
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
