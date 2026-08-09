#!/bin/bash
# Run the full suite (excl. HostResolverTest) multiple times and verify the
# process ALWAYS exits promptly (the fixed bug hung at AtExit teardown after
# the final test, so a non-zero/timeout exit = regression).
SRC=/mnt/c/Personal/Projects/LibNei/libnei-src
BUILD=$SRC/build/linux-gcc-release-shared
BIN=$BUILD/tests/nei_tests
export LD_LIBRARY_PATH=$BUILD/modules/nei:$BUILD/modules/neixx:$BUILD/3rdparty

ROUNDS=${1:-8}
pass=0
hang=0
for ((i=1; i<=ROUNDS; i++)); do
  log="/tmp/shutdown_fix_round_$i.log"
  timeout 180 "$BIN" --gtest_filter='-HostResolverTest.*' > "$log" 2>&1
  rc=$?
  ok=$(grep -c "\[  PASSED  \]" "$log")
  if [ $rc -eq 0 ] && [ "$ok" -ge 1 ]; then
    echo "round $i: PASS (rc=$rc, all passed)"
    pass=$((pass+1))
  else
    echo "round $i: HANG/FAIL (rc=$rc)"
    tail -3 "$log"
    hang=$((hang+1))
  fi
done
echo "=============================="
echo "RESULT: $pass/$ROUNDS passed, $hang hung/failed"
