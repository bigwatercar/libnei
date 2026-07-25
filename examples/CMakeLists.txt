add_executable(log_example log_example.c)
target_link_libraries(log_example PRIVATE nei::nei)

add_executable(log_immediate_crash_example log_immediate_crash_example.c)
target_link_libraries(log_immediate_crash_example PRIVATE nei::nei)

add_executable(string_util_example string_util_example.cpp)
target_link_libraries(string_util_example PRIVATE nei::nei)

add_executable(command_line_example command_line_example.cpp)
target_link_libraries(command_line_example PRIVATE nei::nei)

add_executable(flake_id_example flake_id_example.c)
target_link_libraries(flake_id_example PRIVATE nei::nei)

add_executable(message_pump_io_example message_pump_io_example.cpp)
target_link_libraries(message_pump_io_example PRIVATE nei::nei)
if(WIN32)
  target_link_libraries(message_pump_io_example PRIVATE ws2_32)
endif()

add_executable(async_file_example async_file_example.cpp)
target_link_libraries(async_file_example PRIVATE nei::nei)

add_executable(stream_reader_writer_example stream_reader_writer_example.cpp)
target_link_libraries(stream_reader_writer_example PRIVATE nei::nei)

add_executable(nei_example nei_example.cpp)
target_link_libraries(nei_example PRIVATE nei::nei)

add_executable(at_exit_example at_exit_example.cpp)
target_link_libraries(at_exit_example PRIVATE nei::nei)

add_executable(elevated_process_example elevated_process_example.cpp)
target_link_libraries(elevated_process_example PRIVATE nei::nei)

add_executable(memory_monitor_example memory_monitor_example.cpp)
target_link_libraries(memory_monitor_example PRIVATE nei::nei)

add_executable(resolve_connect_example resolve_connect_example.cpp)
target_link_libraries(resolve_connect_example PRIVATE nei::nei)

if(WIN32)
  add_executable(pipe_stream_cross_process_win_example
                 pipe_stream_cross_process_win_example.cpp)
  target_link_libraries(pipe_stream_cross_process_win_example PRIVATE nei::nei)
elseif(UNIX)
  add_executable(pipe_stream_cross_process_posix_example
                 pipe_stream_cross_process_posix_example.cpp)
  target_link_libraries(pipe_stream_cross_process_posix_example PRIVATE nei::nei)
endif()

if(WIN32 AND BUILD_SHARED_LIBS)
  include(neiTargetUtils)
  nei_copy_runtime_dlls(log_example)
  nei_copy_runtime_dlls(log_immediate_crash_example)
  nei_copy_runtime_dlls(string_util_example)
  nei_copy_runtime_dlls(command_line_example)
  nei_copy_runtime_dlls(flake_id_example)
  nei_copy_runtime_dlls(message_pump_io_example)
  nei_copy_runtime_dlls(async_file_example)
  nei_copy_runtime_dlls(stream_reader_writer_example)
  nei_copy_runtime_dlls(nei_example)
  nei_copy_runtime_dlls(at_exit_example)
  nei_copy_runtime_dlls(pipe_stream_cross_process_win_example)
  nei_copy_runtime_dlls(elevated_process_example)
  nei_copy_runtime_dlls(memory_monitor_example)
  nei_copy_runtime_dlls(resolve_connect_example)

  nei_set_utf8_encoding(log_example)
  nei_set_utf8_encoding(log_immediate_crash_example)
  nei_set_utf8_encoding(string_util_example)
  nei_set_utf8_encoding(command_line_example)
  nei_set_utf8_encoding(flake_id_example)
  nei_set_utf8_encoding(message_pump_io_example)
  nei_set_utf8_encoding(async_file_example)
  nei_set_utf8_encoding(stream_reader_writer_example)
  nei_set_utf8_encoding(nei_example)
  nei_set_utf8_encoding(at_exit_example)
  nei_set_utf8_encoding(pipe_stream_cross_process_win_example)
  nei_set_utf8_encoding(elevated_process_example)
  nei_set_utf8_encoding(memory_monitor_example)
  nei_set_utf8_encoding(resolve_connect_example)
endif()
