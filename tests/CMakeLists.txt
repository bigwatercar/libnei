include(FetchContent)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

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

enable_testing()

add_executable(
  nei_tests
  async_line_reader_test.cpp
  base64_test.cpp
  checksum_hash_test.cpp
  check_macro_test.cpp
  child_process_test.cpp
  cjk_width_test.cpp
  condition_variable_test.cpp
  command_line_test.cpp
  core_test.cpp
  flake_id_test.cpp
  lock_test.cpp
  log_test.cpp
  functional_test.cpp
  # ref_counted_compile_fail.cpp
  platform_thread_test.cpp
  ref_counted_test.cpp
  thread_test.cpp
  message_pump_test.cpp
  message_pump_io_current_test.cpp
  message_pump_default_test.cpp
  sequence_manager_test.cpp
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
  text_normalization_test.cpp
  time_test.cpp
  utf_string_conversions_test.cpp
  utils_crypto_uuid_test.cpp
  xdr_test.cpp
)

target_link_libraries(nei_tests PRIVATE nei::all GTest::gtest_main)

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
target_link_libraries(ref_counted_compile_fail PRIVATE nei::all)

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
