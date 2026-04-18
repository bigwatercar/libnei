target_sources(
    neixx
    PRIVATE
        src/functional/callback_base.cpp
        src/functional/callback.cpp
        src/internal_flag.cpp
        src/task_tracer.cpp
        src/time_source.cpp
        src/task_runner.cpp
        src/single_thread_task_runner.cpp
        src/thread.cpp
        src/thread_pool.cpp
        src/scoped_blocking_call.cpp
        src/task_environment.cpp
        src/sequenced_task_runner.cpp
)

target_include_directories(
    neixx
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

