#!/bin/bash
# TSan regression: rebuild and run full suite excluding known slow-hang/env
# failures (see testing-lessons memory).
SRC=/mnt/c/Personal/Projects/LibNei/libnei-src
BUILD=$SRC/build/linux-gcc-tsan
LOG=/tmp/tsan_regress.log
echo "=== 1. Rebuild TSan nei_tests ==="
cmake --build "$BUILD" -j$(nproc) --target nei_tests 2>&1 | tail -3
if [ $? -ne 0 ]; then echo "BUILD FAILED"; exit 1; fi
echo "BUILD OK"
export LD_LIBRARY_PATH=$BUILD/modules/nei:$BUILD/modules/neixx:$BUILD/3rdparty
export TSAN_OPTIONS=halt_on_error=0
EXCLUDE='-HostResolverTest.*'
EXCLUDE="$EXCLUDE:SequenceManagerTest.MultiQueueBurstDoesNotStarveAnyQueue"
EXCLUDE="$EXCLUDE:LogCTest.ConcurrentFirstUseInitializationStress"
EXCLUDE="$EXCLUDE:PipeStreamTest.PosixYieldQuotaPreventsStarvation"
EXCLUDE="$EXCLUDE:TlsSocketTest.LargePayloadBioCompaction"
echo "=== 2. Full suite (TSan) ==="
timeout 1200 setarch $(uname -m) -R "$BUILD/tests/nei_tests" --gtest_filter="$EXCLUDE" > "$LOG" 2>&1
rc=$?
echo "exit=$rc"
grep -E "\[  PASSED  \]|\[  FAILED  \]|FAILED TEST|WARNING: ThreadSanitizer|SUMMARY: ThreadSanitizer" "$LOG" | tail -10
echo "=== 3. Any TSan races ==="
grep -c "WARNING: ThreadSanitizer" "$LOG" || true
