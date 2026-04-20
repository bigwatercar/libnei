include(FetchContent)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(
    nei_tests
    core_test.cpp
    log_test.cpp
    string_util_test.cpp
    split_string_test.cpp
    string_number_conversions_test.cpp
  text_normalization_test.cpp
  cjk_width_test.cpp
  chinese_search_key_test.cpp
    utf_string_conversions_test.cpp
    cancelable_callback_test.cpp
    task_callback_test.cpp
    task_environment_test.cpp
    task_scheduler_test.cpp
    task_weak_ptr_test.cpp
    task_stress_test.cpp
    scoped_blocking_call_test.cpp
    mixed_load_test.cpp
    scoped_blocking_call_shutdown_test.cpp
)

target_link_libraries(
    nei_tests
    PRIVATE
  nei::all
        GTest::gtest_main
)

include(GoogleTest)
#
# Avoid running the test executable during build time on Windows shared builds
# (DLLs may not be staged yet). Discover tests at test time instead.
#
gtest_discover_tests(nei_tests DISCOVERY_MODE PRE_TEST)

# MSVC + shared libs: put nei.dll and googletest DLLs next to nei_tests.exe (and
# sync googletest DLLs to nei.dll's output dir) so gtest_discover_tests and
# ctest can run the executable without PATH tricks.
if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET nei_tests
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:nei_tests> $<TARGET_FILE_DIR:nei_tests>
    COMMAND_EXPAND_LISTS
  )
  add_custom_command(
    TARGET nei_tests
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:nei_tests> $<TARGET_FILE_DIR:nei>
    COMMAND_EXPAND_LISTS
  )
endif()
