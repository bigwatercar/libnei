target_sources(
  neixx
  PRIVATE include/neixx/task/internal/task.h
          include/neixx/task/scoped_blocking_call.h
          include/neixx/task/thread_pool_instance.h
          src/internal/delayed_task_manager.h
          src/internal/pooled_task_source.h
          include/neixx/task/internal/task_queue.h
          include/neixx/task/message_loop/message_pump.h
          include/neixx/task/message_loop/message_pump_default.h
          include/neixx/task/run_loop.h
          include/neixx/task/sequence_manager.h
          include/neixx/task/task_runner.h
          include/neixx/task/task_traits.h
          include/neixx/task/thread_pool.h
          include/neixx/task/thread_task_runner_handle.h
          src/scoped_blocking_call.cpp
          src/internal/delayed_task_manager.cpp
          src/internal/pooled_task_source.cpp
          src/internal/task_queue.cpp
          src/message_loop/message_pump_default.cpp
          src/run_loop.cpp
          src/sequence_manager.cpp
          src/sequence_token.cpp
          src/task_runner.cpp
          src/thread_pool.cpp
          src/thread_pool_instance.cpp
          src/thread_task_runner_handle.cpp
)

target_include_directories(
  neixx PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
