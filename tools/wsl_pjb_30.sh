#!/bin/bash
# WSL: run post_job_bench 30x, report hangs, then allow ptrace for gdb.
SRC=/mnt/c/Personal/Projects/LibNei/libnei-src
BUILD=$SRC/build/linux-gcc-release-shared
BIN=$BUILD/bench/post_job_bench
export LD_LIBRARY_PATH=$BUILD/modules/nei:$BUILD/modules/neixx:$BUILD/3rdparty
hung=0
for i in $(seq 1 30); do
  "$BIN" > /tmp/pjb30_$i.log 2>&1 &
  pid=$!
  ok=0
  for t in $(seq 1 80); do
    if ! kill -0 $pid 2>/dev/null; then ok=1; break; fi
    sleep 0.5
  done
  if [ "$ok" = "1" ]; then
    printf "."
  else
    echo " HUNG run $i"
    kill -9 $pid 2>/dev/null
    hung=$((hung+1))
  fi
done
echo ""
echo "WSL 30 runs hung=$hung"
echo "--- ptrace ---"
sudo sysctl -w kernel.yama.ptrace_scope=0 2>/dev/null || echo "no sysctl sudo"
cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null || echo "no ptrace_scope file"
