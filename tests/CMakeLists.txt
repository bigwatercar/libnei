include(FetchContent)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

find_package(GTest QUIET)
if(NOT GTest_FOUND)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )

  set(gtest_force_shared_crt
      ON
      CACHE BOOL "" FORCE
  )
  FetchContent_MakeAvailable(googletest)
endif()

enable_testing()

add_executable(
  nei_tests
  test_main.cpp
  file_stream_adapters_test.cpp
  fs_util_test.cpp
  async_file_posix_test.cpp
  async_file_win_test.cpp
  async_line_reader_test.cpp
  stream_reader_writer_test.cpp
  io_buffer_test.cpp
  base64_test.cpp
  buffer_stream_test.cpp
  cancelable_callback_test.cpp
  once_callback_test.cpp
  checksum_hash_test.cpp
  check_macro_test.cpp
  core_time_test.cpp
  child_process_test.cpp
  cjk_width_test.cpp
  condition_variable_test.cpp
  command_line_test.cpp
  core_test.cpp
  flake_id_test.cpp
  lock_test.cpp
  log_test.cpp
  functional_test.cpp
  small_object_allocator_test.cpp
  memory_pressure_monitor_test.cpp
  # ref_counted_compile_fail.cpp
  platform_thread_test.cpp
  pipe_stream_test.cpp
  file_path_watcher_test.cpp
  ref_counted_test.cpp
  weak_ptr_test.cpp
  thread_test.cpp
  simple_thread_test.cpp
  thread_local_test.cpp
  timer_test.cpp
  message_pump_test.cpp
  message_pump_io_current_test.cpp
  message_pump_default_test.cpp
  path_util_test.cpp
  sequence_manager_test.cpp
  shared_memory_test.cpp
  run_loop_test.cpp
  task_sequence_token_test.cpp
  task_traits_test.cpp
  task_internal_task_test.cpp
  task_runner_task_queue_test.cpp
  thread_pool_test.cpp
  thread_local_storage_test.cpp
  split_string_test.cpp
  string_number_conversions_test.cpp
  string_util_test.cpp
  sys_info_test.cpp
  text_normalization_test.cpp
  time_test.cpp
  utf_string_conversions_test.cpp
  utils_crypto_uuid_test.cpp
  win_utils_test.cpp
  xdr_test.cpp
)

add_subdirectory(net)

target_link_libraries(nei_tests PRIVATE nei::nei GTest::gtest mbedtls)
if(MSVC)
  target_compile_options(nei_tests PRIVATE /FS) # /FS: force synchronous PDB
                                                # writes (avoids C1041)
endif()
target_include_directories(
  nei_tests
  PRIVATE ${PROJECT_SOURCE_DIR}/modules/neixx/io/src
          ${PROJECT_SOURCE_DIR}/modules/neixx/process/src
          ${PROJECT_SOURCE_DIR}/modules/neixx/net/src
          ${PROJECT_SOURCE_DIR}/modules/neixx/task/src
          ${PROJECT_SOURCE_DIR}/3rdparty/mbedtls/include
)

include(GoogleTest)
#
# Avoid running the test executable during build time on Windows shared builds
# (DLLs may not be staged yet). Discover tests at test time instead.
#
gtest_discover_tests(nei_tests DISCOVERY_MODE PRE_TEST)

# Optional compile-fail check for scoped_refptr constraints. This target is
# excluded from default builds and is intended to be invoked manually to inspect
# diagnostic readability.
add_executable(
  ref_counted_compile_fail EXCLUDE_FROM_ALL ref_counted_compile_fail.cpp
)
target_link_libraries(ref_counted_compile_fail PRIVATE nei::nei)

add_executable(post_job_smoke_test EXCLUDE_FROM_ALL post_job_smoke_test.cpp)
target_link_libraries(post_job_smoke_test PRIVATE nei::nei)

# MSVC + shared libs: put nei.dll and googletest DLLs next to nei_tests.exe (and
# sync googletest DLLs to nei.dll's output dir) so gtest_discover_tests and
# ctest can run the executable without PATH tricks.
if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET nei_tests
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:nei_tests> $<TARGET_FILE_DIR:nei_tests>
    COMMAND_EXPAND_LISTS
  )
  add_custom_command(
    TARGET nei_tests
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:nei_tests> $<TARGET_FILE_DIR:nei>
    COMMAND_EXPAND_LISTS
  )
endif()
