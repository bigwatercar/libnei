add_executable(log_demo log_demo.c)
target_link_libraries(log_demo PRIVATE nei::nei)

add_executable(log_immediate_crash_demo log_immediate_crash_demo.c)
target_link_libraries(log_immediate_crash_demo PRIVATE nei::nei)

add_executable(string_util_demo string_util_demo.cpp)
target_link_libraries(string_util_demo PRIVATE nei::nei)

add_executable(command_line_demo command_line_demo.cpp)
target_link_libraries(command_line_demo PRIVATE nei::nei)

add_executable(flake_id_demo flake_id_demo.c)
target_link_libraries(flake_id_demo PRIVATE nei::nei)

add_executable(message_pump_io_demo message_pump_io_demo.cpp)
target_link_libraries(message_pump_io_demo PRIVATE nei::nei)
if(WIN32)
  target_link_libraries(message_pump_io_demo PRIVATE ws2_32)
endif()

add_executable(async_file_demo async_file_demo.cpp)
target_link_libraries(async_file_demo PRIVATE nei::nei)

add_executable(stream_reader_writer_demo stream_reader_writer_demo.cpp)
target_link_libraries(stream_reader_writer_demo PRIVATE nei::nei)

add_executable(nei_demo nei_demo.cpp)
target_link_libraries(nei_demo PRIVATE nei::nei)

add_executable(at_exit_demo at_exit_demo.cpp)
target_link_libraries(at_exit_demo PRIVATE nei::nei)

add_executable(elevated_process_demo elevated_process_demo.cpp)
target_link_libraries(elevated_process_demo PRIVATE nei::nei)

add_executable(memory_monitor_demo memory_monitor_demo.cpp)
target_link_libraries(memory_monitor_demo PRIVATE nei::nei)

add_executable(resolve_connect_demo resolve_connect_demo.cpp)
target_link_libraries(resolve_connect_demo PRIVATE nei::nei)

if(WIN32)
  add_executable(pipe_stream_cross_process_win_demo
                 pipe_stream_cross_process_win_demo.cpp)
  target_link_libraries(pipe_stream_cross_process_win_demo PRIVATE nei::nei)
elseif(UNIX)
  add_executable(pipe_stream_cross_process_posix_demo
                 pipe_stream_cross_process_posix_demo.cpp)
  target_link_libraries(pipe_stream_cross_process_posix_demo PRIVATE nei::nei)
endif()

if(WIN32 AND BUILD_SHARED_LIBS)
  include(neiTargetUtils)
  nei_copy_runtime_dlls(log_demo)
  nei_copy_runtime_dlls(log_immediate_crash_demo)
  nei_copy_runtime_dlls(string_util_demo)
  nei_copy_runtime_dlls(command_line_demo)
  nei_copy_runtime_dlls(flake_id_demo)
  nei_copy_runtime_dlls(message_pump_io_demo)
  nei_copy_runtime_dlls(async_file_demo)
  nei_copy_runtime_dlls(stream_reader_writer_demo)
  nei_copy_runtime_dlls(nei_demo)
  nei_copy_runtime_dlls(at_exit_demo)
  nei_copy_runtime_dlls(pipe_stream_cross_process_win_demo)
  nei_copy_runtime_dlls(elevated_process_demo)
  nei_copy_runtime_dlls(memory_monitor_demo)
  nei_copy_runtime_dlls(resolve_connect_demo)

  nei_set_utf8_encoding(log_demo)
  nei_set_utf8_encoding(log_immediate_crash_demo)
  nei_set_utf8_encoding(string_util_demo)
  nei_set_utf8_encoding(command_line_demo)
  nei_set_utf8_encoding(flake_id_demo)
  nei_set_utf8_encoding(message_pump_io_demo)
  nei_set_utf8_encoding(async_file_demo)
  nei_set_utf8_encoding(stream_reader_writer_demo)
  nei_set_utf8_encoding(nei_demo)
  nei_set_utf8_encoding(at_exit_demo)
  nei_set_utf8_encoding(pipe_stream_cross_process_win_demo)
  nei_set_utf8_encoding(elevated_process_demo)
  nei_set_utf8_encoding(memory_monitor_demo)
  nei_set_utf8_encoding(resolve_connect_demo)
endif()
