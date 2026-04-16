include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(
    nei_tests
    core_test.cpp
    log_test.cpp
    task_callback_test.cpp
    task_environment_test.cpp
    task_scheduler_test.cpp
    scoped_blocking_call_test.cpp
    mixed_load_test.cpp
    scoped_blocking_call_shutdown_test.cpp
)

target_link_libraries(
    nei_tests
    PRIVATE
        nei::nei
        GTest::gtest_main
)

add_executable(log_bench log_bench.cpp)
target_link_libraries(log_bench PRIVATE nei::nei)

option(NEI_BENCHMARK_SPDLOG "Build log_bench_compare (NEI vs spdlog, FetchContent spdlog)" ON)
if(NEI_BENCHMARK_SPDLOG)
  FetchContent_Declare(
    spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.0.zip
  )
  set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(spdlog)

  add_executable(log_bench_compare log_bench_compare.cpp)
  target_link_libraries(log_bench_compare PRIVATE nei::nei spdlog::spdlog)
endif()

add_executable(log_test2 log_test2.c)
target_link_libraries(log_test2 PRIVATE nei::nei)

add_executable(task_thread_demo task_thread_demo.cpp)
target_link_libraries(task_thread_demo PRIVATE nei::nei)

add_executable(task_weak_ptr_demo task_weak_ptr_demo.cpp)
target_link_libraries(task_weak_ptr_demo PRIVATE nei::nei)

add_executable(task_location_delay_demo task_location_delay_demo.cpp)
target_link_libraries(task_location_delay_demo PRIVATE nei::nei)

add_executable(task_priority_demo task_priority_demo.cpp)
target_link_libraries(task_priority_demo PRIVATE nei::nei)

add_executable(task_shutdown_demo task_shutdown_demo.cpp)
target_link_libraries(task_shutdown_demo PRIVATE nei::nei)

add_executable(task_may_block_demo task_may_block_demo.cpp)
target_link_libraries(task_may_block_demo PRIVATE nei::nei)

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
  add_custom_command(
    TARGET log_test2
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:log_test2> $<TARGET_FILE_DIR:log_test2>
    COMMAND_EXPAND_LISTS
  )
  add_custom_command(
    TARGET task_thread_demo
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:task_thread_demo> $<TARGET_FILE_DIR:task_thread_demo>
    COMMAND_EXPAND_LISTS
  )
  add_custom_command(
    TARGET task_weak_ptr_demo
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:task_weak_ptr_demo> $<TARGET_FILE_DIR:task_weak_ptr_demo>
    COMMAND_EXPAND_LISTS
  )
  add_custom_command(
    TARGET task_location_delay_demo
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:task_location_delay_demo> $<TARGET_FILE_DIR:task_location_delay_demo>
    COMMAND_EXPAND_LISTS
  )
  add_custom_command(
    TARGET task_priority_demo
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:task_priority_demo> $<TARGET_FILE_DIR:task_priority_demo>
    COMMAND_EXPAND_LISTS
  )
  add_custom_command(
    TARGET task_shutdown_demo
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:task_shutdown_demo> $<TARGET_FILE_DIR:task_shutdown_demo>
    COMMAND_EXPAND_LISTS
  )
  add_custom_command(
    TARGET task_may_block_demo
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:task_may_block_demo> $<TARGET_FILE_DIR:task_may_block_demo>
    COMMAND_EXPAND_LISTS
  )
  if(NEI_BENCHMARK_SPDLOG AND TARGET log_bench_compare)
    add_custom_command(
      TARGET log_bench_compare
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:log_bench_compare> $<TARGET_FILE_DIR:log_bench_compare>
      COMMAND_EXPAND_LISTS
    )
  endif()
endif()
