#!/bin/bash
# WSL verification for the PooledTaskSource::Shutdown lost-wakeup fix.
SRC=/mnt/c/Personal/Projects/LibNei/libnei-src
BUILD=$SRC/build/linux-gcc-release-shared
BIN=$BUILD/tests/nei_tests
export LD_LIBRARY_PATH=$BUILD

echo "=== 1. Build nei_tests (Release shared) ==="
cmake --build "$BUILD" -j$(nproc) --target nei_tests 2>&1 | tail -5
if [ $? -ne 0 ]; then echo "BUILD FAILED"; exit 1; fi
echo "BUILD OK"

echo ""
echo "=== 2. New regression test ==="
"$BIN" --gtest_filter=ThreadPoolTest.RepeatedShutdownAfterWorkerIdleWaitNeverHangs 2>&1 | grep -E "\[(  PASSED|  FAILED| RUN|       OK|  FAILED  )" | tail -5

echo ""
echo "=== 3. Task suite regression ==="
"$BIN" --gtest_filter='ThreadPoolTest.*:SequenceManagerTest.*:TimerTest.*:JobTest.*:TaskRunnerTest.*:TaskQueueTest.*' 2>&1 | grep -E "\[  PASSED  \]|\[  FAILED  \]|FAILED TEST" | tail -5

echo ""
echo "=== 4. Full suite (exclude HostResolverTest - WSL env) ==="
"$BIN" --gtest_filter='-HostResolverTest.*' 2>&1 | grep -E "\[  PASSED  \]|\[  FAILED  \]|FAILED TEST|tests from .* test suites ran" | tail -8
