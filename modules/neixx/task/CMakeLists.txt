set(NEIXX_TASK_SOURCES
  include/neixx/task/bind_post_task.h
  include/neixx/task/task_tracing.h
  include/neixx/task/message_loop/message_pump_default.h
  include/neixx/task/message_loop/message_pump_io.h
  include/neixx/task/message_loop/message_pump_type.h
  include/neixx/task/message_loop/message_pump.h
  include/neixx/task/run_loop.h
  include/neixx/task/scoped_blocking_call.h
  include/neixx/task/sequence_checker.h
  include/neixx/task/sequence_manager.h
  include/neixx/task/sequence_token.h
  include/neixx/task/task_observer.h
  include/neixx/task/task_runner.h
  include/neixx/task/task_traits.h
  include/neixx/task/thread_checker.h
  include/neixx/task/job_delegate.h
  include/neixx/task/post_job.h
  include/neixx/task/thread_pool_instance.h
  include/neixx/task/thread_pool.h
  include/neixx/task/thread_task_runner_handle.h
  include/neixx/task/timer.h
  src/internal/delayed_task_manager.cpp
  src/internal/delayed_task_manager.h
  src/internal/job_task_source.cpp
  src/internal/job_task_source.h
  src/internal/pooled_task_runner_utils.cpp
  src/internal/pooled_task_runner_utils.h
  src/internal/pooled_task_source.cpp
  src/internal/pooled_task_source.h
  src/internal/task.h
  src/internal/task_queue.cpp
  src/internal/task_queue.h
  src/internal/task_queue_selector.cpp
  src/internal/task_queue_selector.h
  src/internal/task_source.cpp
  src/internal/task_source.h
  src/internal/task_tracker.cpp
  src/internal/task_tracker.h
  src/internal/task_tracing.cpp
  src/internal/task_tracing_internal.h
  src/message_loop/message_pump_default.cpp
  src/message_loop/message_pump_default.h
  src/post_job.cpp
  src/run_loop.cpp
  src/scoped_blocking_call.cpp
  src/sequence_manager.cpp
  src/sequence_token.cpp
  src/task_runner.cpp
  src/thread_pool_instance.cpp
  src/thread_pool.cpp
  src/thread_task_runner_handle.cpp
  src/timer.cpp
)

if(WIN32)
  list(APPEND NEIXX_TASK_SOURCES src/message_loop/message_pump_io_win.cpp)
else()
  list(APPEND NEIXX_TASK_SOURCES src/message_loop/message_pump_io_posix.cpp)
endif()

target_sources(nei PRIVATE ${NEIXX_TASK_SOURCES})

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
