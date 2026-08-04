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

add_executable(task_threadpool_parallel_bench task_threadpool_parallel_bench.cpp)
target_link_libraries(task_threadpool_parallel_bench PRIVATE nei::nei)

add_executable(post_job_bench post_job_bench.cpp)
target_link_libraries(post_job_bench PRIVATE nei::nei)

add_executable(parallel_runner_bench parallel_runner_bench.cpp)
target_link_libraries(parallel_runner_bench PRIVATE nei::nei)

add_executable(task_priority_perf_demo task_priority_perf_demo.cpp)
target_link_libraries(task_priority_perf_demo PRIVATE nei::nei)

add_executable(callback_bench callback_bench.cpp)
target_link_libraries(callback_bench PRIVATE nei::nei)

add_executable(async_file_bench async_file_bench.cpp)
target_link_libraries(async_file_bench PRIVATE nei::nei)

add_executable(pipe_stream_bench pipe_stream_bench.cpp)
target_link_libraries(pipe_stream_bench PRIVATE nei::nei)

add_executable(tcp_loopback_bench tcp_loopback_bench.cpp)
target_link_libraries(tcp_loopback_bench PRIVATE nei::nei)
if(WIN32)
  target_link_libraries(tcp_loopback_bench PRIVATE ws2_32)
endif()

add_executable(tcp_conn_stress_bench tcp_conn_stress_bench.cpp)
target_link_libraries(tcp_conn_stress_bench PRIVATE nei::nei)
if(WIN32)
  target_link_libraries(tcp_conn_stress_bench PRIVATE ws2_32)
endif()

add_executable(tcp_rtt_bench tcp_rtt_bench.cpp)
target_link_libraries(tcp_rtt_bench PRIVATE nei::nei)
if(WIN32)
  target_link_libraries(tcp_rtt_bench PRIVATE ws2_32)
endif()

add_executable(tcp_cross_bench tcp_cross_bench.cpp)
target_link_libraries(tcp_cross_bench PRIVATE nei::nei)
if(WIN32)
  target_link_libraries(tcp_cross_bench PRIVATE ws2_32)
endif()

add_executable(tcp_throughput_bench tcp_throughput_bench.cpp)
target_link_libraries(tcp_throughput_bench PRIVATE nei::nei)
if(WIN32)
  target_link_libraries(tcp_throughput_bench PRIVATE ws2_32)
endif()

add_executable(tls_throughput_bench tls_throughput_bench.cpp)
target_link_libraries(tls_throughput_bench PRIVATE nei::nei mbedtls mbedx509 mbedcrypto)
if(WIN32)
  target_link_libraries(tls_throughput_bench PRIVATE ws2_32)
endif()

if(WIN32)
  add_executable(pipe_stream_cross_process_bench
                 pipe_stream_cross_process_bench_win.cpp)
  target_link_libraries(pipe_stream_cross_process_bench PRIVATE nei::nei)
elseif(UNIX)
  add_executable(pipe_stream_cross_process_bench
                 pipe_stream_cross_process_bench_posix.cpp)
  target_link_libraries(pipe_stream_cross_process_bench PRIVATE nei::nei)
endif()

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
  include(neiTargetUtils)
  nei_copy_runtime_dlls(log_bench)
  nei_copy_runtime_dlls(string_append_bench)
  nei_copy_runtime_dlls(flake_id_bench)
  nei_copy_runtime_dlls(task_thread_bench)
  nei_copy_runtime_dlls(task_threadpool_bench)
  nei_copy_runtime_dlls(task_threadpool_parallel_bench)
  nei_copy_runtime_dlls(task_priority_perf_demo)
  nei_copy_runtime_dlls(callback_bench)
  nei_copy_runtime_dlls(async_file_bench)
  nei_copy_runtime_dlls(pipe_stream_bench)
  nei_copy_runtime_dlls(pipe_stream_cross_process_bench)
  nei_copy_runtime_dlls(tcp_loopback_bench)
  nei_copy_runtime_dlls(tcp_conn_stress_bench)
  nei_copy_runtime_dlls(tcp_rtt_bench)
  nei_copy_runtime_dlls(tcp_cross_bench)
  nei_copy_runtime_dlls(tcp_throughput_bench)
  nei_copy_runtime_dlls(tls_throughput_bench)

  nei_set_utf8_encoding(log_bench)
  nei_set_utf8_encoding(string_append_bench)
  nei_set_utf8_encoding(flake_id_bench)
  nei_set_utf8_encoding(task_thread_bench)
  nei_set_utf8_encoding(task_threadpool_bench)
  nei_set_utf8_encoding(task_threadpool_parallel_bench)
  nei_set_utf8_encoding(task_priority_perf_demo)
  nei_set_utf8_encoding(callback_bench)
  nei_set_utf8_encoding(async_file_bench)
  nei_set_utf8_encoding(pipe_stream_bench)
  nei_set_utf8_encoding(pipe_stream_cross_process_bench)
  nei_set_utf8_encoding(tcp_loopback_bench)
  nei_set_utf8_encoding(tcp_conn_stress_bench)
  nei_set_utf8_encoding(tcp_rtt_bench)
  nei_set_utf8_encoding(tcp_cross_bench)
  nei_set_utf8_encoding(tcp_throughput_bench)
  nei_set_utf8_encoding(tls_throughput_bench)

  if(NEI_BENCHMARK_SPDLOG AND TARGET log_bench_compare)
    nei_copy_runtime_dlls(log_bench_compare)
    nei_set_utf8_encoding(log_bench_compare)
  endif()
endif()
