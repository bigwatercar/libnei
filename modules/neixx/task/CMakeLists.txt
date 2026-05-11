target_sources(
  neixx
  PRIVATE include/neixx/task/internal/task.h
          include/neixx/task/internal/task_queue.h
          include/neixx/task/message_loop/message_pump.h
          include/neixx/task/message_loop/message_pump_default.h
          include/neixx/task/sequence_manager.h
          include/neixx/task/task_runner.h
          include/neixx/task/task_traits.h
          src/internal/task_queue.cpp
          src/message_loop/message_pump_default.cpp
          src/sequence_manager.cpp
          src/sequence_token.cpp
          src/task_runner.cpp
)

target_include_directories(
  neixx PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
