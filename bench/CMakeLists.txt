include(FetchContent)

option(NEI_BENCHMARK_FMT
       "Build log_bench with {fmt} for C vs fmt formatting comparison" ON
)
add_executable(log_bench log_bench.cpp)
target_link_libraries(log_bench PRIVATE nei::nei)
if(NEI_BENCHMARK_FMT)
  FetchContent_Declare(
    fmt URL https://github.com/fmtlib/fmt/archive/refs/tags/11.1.3.zip
               DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(FMT_INSTALL
      OFF
      CACHE BOOL "" FORCE
  )
  set(FMT_TEST
      OFF
      CACHE BOOL "" FORCE
  )
  set(FMT_DOC
      OFF
      CACHE BOOL "" FORCE
  )
  FetchContent_MakeAvailable(fmt)
  target_link_libraries(log_bench PRIVATE fmt::fmt)
  target_compile_definitions(log_bench PRIVATE NEI_BENCH_HAS_FMT=1)
endif()

add_executable(string_append_bench string_append_bench.cpp)
target_link_libraries(string_append_bench PRIVATE nei::nei)

add_executable(flake_id_bench flake_id_bench.c)
target_link_libraries(flake_id_bench PRIVATE nei::nei)

add_executable(task_thread_bench task_thread_bench.cpp)
target_link_libraries(task_thread_bench PRIVATE nei::nei)

add_executable(task_threadpool_bench task_threadpool_bench.cpp)
target_link_libraries(task_threadpool_bench PRIVATE nei::nei)

add_executable(task_priority_perf_demo task_priority_perf_demo.cpp)
target_link_libraries(task_priority_perf_demo PRIVATE nei::nei)

add_executable(async_file_bench async_file_bench.cpp)
target_link_libraries(async_file_bench PRIVATE nei::nei)

option(NEI_BENCHMARK_SPDLOG
       "Build log_bench_compare (NEI vs spdlog, FetchContent spdlog)" ON
)
if(NEI_BENCHMARK_SPDLOG)
  FetchContent_Declare(
    spdlog URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.0.zip
               DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(SPDLOG_BUILD_EXAMPLE
      OFF
      CACHE BOOL "" FORCE
  )
  set(SPDLOG_BUILD_TESTS
      OFF
      CACHE BOOL "" FORCE
  )
  set(SPDLOG_BUILD_BENCH
      OFF
      CACHE BOOL "" FORCE
  )
  FetchContent_MakeAvailable(spdlog)

  add_executable(log_bench_compare log_bench_compare.cpp)
  target_link_libraries(log_bench_compare PRIVATE nei::nei spdlog::spdlog fmt::fmt)

  if(BUILD_TESTING AND NEI_ENABLE_LONG_BENCH_TESTS)
    add_test(NAME LogBenchCompareLong COMMAND log_bench_compare)
    set_tests_properties(
      LogBenchCompareLong PROPERTIES LABELS "benchmark;long" TIMEOUT 1800
    )
  endif()
endif()

if(WIN32 AND BUILD_SHARED_LIBS)
  function(nei_copy_runtime_dlls target_name)
    add_custom_command(
      TARGET ${target_name}
      POST_BUILD
      COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:${target_name}> $<TARGET_FILE_DIR:${target_name}>
      COMMAND_EXPAND_LISTS
    )
  endfunction()

  nei_copy_runtime_dlls(log_bench)
  nei_copy_runtime_dlls(string_append_bench)
  nei_copy_runtime_dlls(flake_id_bench)
  nei_copy_runtime_dlls(task_thread_bench)
  nei_copy_runtime_dlls(task_threadpool_bench)
  nei_copy_runtime_dlls(task_priority_perf_demo)
  nei_copy_runtime_dlls(async_file_bench)

  if(NEI_BENCHMARK_SPDLOG AND TARGET log_bench_compare)
    nei_copy_runtime_dlls(log_bench_compare)
  endif()
endif()
