#!/bin/bash
# valgrind memcheck regression on key task/ExecutionFence tests.
# Excludes timing-sensitive tests known to deadlock under valgrind.
SRC=/mnt/c/Personal/Projects/LibNei/libnei-src
BUILD=$SRC/build/linux-gcc-release-shared
BIN=$BUILD/tests/nei_tests
LOG=/tmp/vg_regress.log
export LD_LIBRARY_PATH=$BUILD/modules/nei:$BUILD/modules/neixx:$BUILD/3rdparty
FILTER='ThreadPoolTest.ExecutionFence*:ThreadPoolTest.SequencedTaskRunnerSerializesExecution'
FILTER="$FILTER:ThreadPoolTest.PostTaskWakesSleepingWorker"
FILTER="$FILTER:ThreadPoolTest.PostingFromRunningTaskStillExecutesFollowupTask"
FILTER="$FILTER:ThreadPoolTest.DelayedTaskRunsWithoutImmediateKick"
FILTER="$FILTER:ThreadPoolTest.EarlierDelayedTaskPreemptsTimerWait"
FILTER="$FILTER:ThreadPoolTest.MayBlockTasksAllCompleteWithCompensation"
FILTER="$FILTER:TimerTest.RepeatingTimerStopPreventsFurtherFires"
echo "=== valgrind memcheck (key task tests) ==="
timeout 600 valgrind --tool=memcheck --leak-check=no --track-origins=yes --error-exitcode=99 \
  "$BIN" --gtest_filter="$FILTER" > "$LOG" 2>&1
rc=$?
echo "exit=$rc (99 = valgrind found errors)"
grep -E "\[  PASSED  \]|\[  FAILED  \]|FAILED TEST|ERROR SUMMARY|Invalid (read|write)|Use of uninitialised" "$LOG" | tail -8
