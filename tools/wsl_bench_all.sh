#!/bin/bash
# WSL full benchmark collection (mirrors bench/run_all_benches.ps1 params).
SRC=/mnt/c/Personal/Projects/LibNei/libnei-src
BUILD=$SRC/build/linux-gcc-release-shared
BIN=$BUILD/bench
OUT=/tmp/wsl_bench_$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUT"
export LD_LIBRARY_PATH=$BUILD/modules/nei:$BUILD/modules/neixx:$BUILD/3rdparty

echo "=== 0. Build bench targets ==="
cmake --build "$BUILD" -j$(nproc) --target log_bench log_bench_compare string_append_bench flake_id_bench \
  task_thread_bench task_threadpool_bench task_threadpool_parallel_bench post_job_bench parallel_runner_bench \
  task_priority_perf_demo callback_bench async_file_bench pipe_stream_bench pipe_stream_cross_process_bench \
  tcp_loopback_bench tcp_conn_stress_bench tcp_rtt_bench tcp_throughput_bench tls_throughput_bench 2>&1 | tail -3
echo "BUILD OK"
echo "OUT: $OUT"

run_multi() { # name exe rounds args...
  local name=$1 exe=$2 rounds=$3; shift 3
  local log="$OUT/$name.log"
  for r in $(seq 1 "$rounds"); do
    echo "--- $name round $r/$rounds ---" >> "$log"
    "$BIN/$exe" "$@" >> "$log" 2>&1
  done
}

echo "=== 1. task_thread (1M, tracing on/off, 10x) ==="
run_multi task_thread_tracing_on  task_thread_bench 10 1000000 on
run_multi task_thread_tracing_off task_thread_bench 10 1000000 off
echo "=== 2. task_threadpool (1M, 5x) ==="
run_multi task_threadpool task_threadpool_bench 5 1000000 off
echo "=== 3. task_threadpool_parallel (5x) ==="
run_multi task_threadpool_parallel task_threadpool_parallel_bench 5
echo "=== 4. log benches (5x) ==="
mkdir -p "$OUT/log_temp"
run_multi log_bench log_bench 5 "$OUT/log_temp"
run_multi log_bench_compare log_bench_compare 5 "$OUT/log_temp"
echo "=== 5. single-run benches ==="
for exe in post_job_bench flake_id_bench string_append_bench callback_bench async_file_bench pipe_stream_bench \
  pipe_stream_cross_process_bench tcp_loopback_bench tcp_rtt_bench tcp_throughput_bench tcp_conn_stress_bench \
  tls_throughput_bench parallel_runner_bench task_priority_perf_demo; do
  "$BIN/$exe" > "$OUT/$exe.log" 2>&1
done
rm -rf "$OUT/log_temp"
echo "ALL DONE -> $OUT"
ls -la "$OUT" | tail -25
