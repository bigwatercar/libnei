#!/bin/bash
# Reproduce post_job_bench hang on WSL and capture thread stacks via gdb.
SRC=/mnt/c/Personal/Projects/LibNei/libnei-src
BUILD=$SRC/build/linux-gcc-release-shared
BIN=$BUILD/bench/post_job_bench
export LD_LIBRARY_PATH=$BUILD
command -v gdb >/dev/null 2>&1 || { echo "NO gdb"; exit 1; }

for i in $(seq 1 8); do
  "$BIN" > /tmp/pjb_w$i.log 2>&1 &
  pid=$!
  hung=0
  for t in $(seq 1 60); do
    if ! kill -0 $pid 2>/dev/null; then break; fi
    sleep 0.5
  done
  if kill -0 $pid 2>/dev/null; then
    echo "run $i: HUNG (pid=$pid) -> capturing stacks"
    gdb -p $pid -batch -ex "set pagination off" -ex "thread apply all bt 12" > /tmp/pjb_w$i.stack 2>&1
    kill -9 $pid 2>/dev/null
  else
    echo "run $i: ok (exit)"
  fi
done
echo "=== stack files ==="
ls -la /tmp/pjb_w*.stack 2>/dev/null
