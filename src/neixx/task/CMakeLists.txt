set(NEIXX_TASK_SOURCES
    ${PROJECT_SOURCE_DIR}/include/neixx/task/bind_post_task.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/task_tracing.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/message_loop/message_pump_default.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/message_loop/message_pump_io.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/message_loop/message_pump_type.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/message_loop/message_pump.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/run_loop.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/scoped_blocking_call.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/sequence_checker.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/sequence_manager.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/sequence_token.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/task_observer.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/task_runner.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/task_traits.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/thread_checker.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/current_support.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/job_delegate.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/post_job.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/thread_pool_instance.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/thread_pool.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/thread_task_runner_handle.h
    ${PROJECT_SOURCE_DIR}/include/neixx/task/timer.h
    internal/delayed_task_manager.cpp
    internal/delayed_task_manager.h
    internal/job_task_source.cpp
    internal/job_task_source.h
    internal/pooled_task_queue.cpp
    internal/pooled_task_queue.h
    internal/pooled_task_runner_utils.cpp
    internal/pooled_task_runner_utils.h
    internal/pooled_task_source.cpp
    internal/pooled_task_source.h
    internal/sequenced_task_queue.cpp
    internal/sequenced_task_queue.h
    internal/task.h
    internal/task_queue_selector.cpp
    internal/task_queue_selector.h
    internal/task_source.cpp
    internal/task_source.h
    internal/task_source_sort_key.h
    internal/registered_task_source.h
    internal/task_tracker.cpp
    internal/task_tracker.h
    internal/task_tracing.cpp
    internal/task_tracing_internal.h
    message_loop/message_pump_default.cpp
    message_loop/message_pump_default.h
    current_support.cpp
    post_job.cpp
    run_loop.cpp
    scoped_blocking_call.cpp
    sequence_manager.cpp
    sequence_token.cpp
    task_runner.cpp
    thread_pool_instance.cpp
    thread_pool.cpp
    thread_task_runner_handle.cpp
    timer.cpp
)

if(WIN32)
  list(APPEND NEIXX_TASK_SOURCES message_loop/message_pump_io_win.cpp)
else()
  list(APPEND NEIXX_TASK_SOURCES message_loop/message_pump_io_posix.cpp)
endif()

target_sources(nei PRIVATE ${NEIXX_TASK_SOURCES})

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

