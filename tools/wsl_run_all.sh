#!/bin/bash
# WSL: run all libnei benches with the same methodology as run_all_benches.ps1.
# Saves raw logs under bench/results/wsl_<timestamp>/.
SRC=/mnt/c/Personal/Projects/LibNei/libnei-src
BUILD=$SRC/build/linux-gcc-release-shared
BIN=$BUILD/bench
OUT=$SRC/bench/results/wsl_$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUT"

run() {
  local name=$1 exe=$2 rounds=$3
  shift 3
  for r in $(seq 1 "$rounds"); do
    echo "=== $name === Round $r / $rounds ===" >> "$OUT/$name.log"
    "$BIN/$exe" "$@" >> "$OUT/$name.log" 2>&1
  done
}

# Task benches (same methodology as Windows)
run task_thread_tracing_on  task_thread_bench 10 1000000 on
run task_thread_tracing_off task_thread_bench 10 1000000 off
run task_threadpool         task_threadpool_bench 5 1000000 off
run task_threadpool_parallel task_threadpool_parallel_bench 5

# Log benches (need a temp dir arg)
LOGDIR="$OUT/log_temp"; mkdir -p "$LOGDIR"
run log_bench        log_bench 5 "$LOGDIR"
run log_bench_compare log_bench_compare 5 "$LOGDIR"

# Single-run benches
run post_job                  post_job_bench 1
run flake_id                  flake_id_bench 1
run string_append             string_append_bench 1
run callback                  callback_bench 1
run async_file                async_file_bench 1
run pipe_stream               pipe_stream_bench 1
run pipe_stream_cross_process pipe_stream_cross_process_bench 1
run tcp_loopback              tcp_loopback_bench 1
run tcp_rtt                   tcp_rtt_bench 1
run tcp_throughput            tcp_throughput_bench 1
run tcp_conn_stress           tcp_conn_stress_bench 1
run tls_throughput            tls_throughput_bench 1
run parallel_runner           parallel_runner_bench 1
run task_priority_perf        task_priority_perf_demo 1

rm -rf "$LOGDIR"
echo "WSL bench logs: $OUT"
