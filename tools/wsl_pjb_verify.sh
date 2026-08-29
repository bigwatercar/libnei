#!/bin/bash
# Verify post_job_bench no longer hangs: rebuild + run N times with timeout.
SRC=/mnt/c/Personal/Projects/LibNei/libnei-src
BUILD=$SRC/build/linux-gcc-release-shared
BIN=$BUILD/bench/post_job_bench
export LD_LIBRARY_PATH=$BUILD
echo "=== rebuild ==="
cmake --build "$BUILD" -j$(nproc) --target post_job_bench 2>&1 | tail -1 || exit 1
ROUNDS=${1:-15}
hung=0
for i in $(seq 1 "$ROUNDS"); do
  "$BIN" > /tmp/pjb_v$i.log 2>&1 &
  pid=$!
  ok=0
  for t in $(seq 1 80); do
    if ! kill -0 $pid 2>/dev/null; then ok=1; break; fi
    sleep 0.5
  done
  if [ "$ok" = "1" ]; then
    echo "run $i: ok"
  else
    echo "run $i: HUNG"
    kill -9 $pid 2>/dev/null
    hung=$((hung+1))
  fi
done
echo "TOTAL hung: $hung/$ROUNDS"
