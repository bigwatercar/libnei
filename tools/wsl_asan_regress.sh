#!/bin/bash
# ASAN regression: rebuild and run full suite (memory errors only).
SRC=/mnt/c/Personal/Projects/LibNei/libnei-src
BUILD=$SRC/build/linux-gcc-debug-asan
LOG=/tmp/asan_regress.log
echo "=== 1. Rebuild ASAN nei_tests ==="
cmake --build "$BUILD" -j$(nproc) --target nei_tests 2>&1 | tail -3
if [ $? -ne 0 ]; then echo "BUILD FAILED"; exit 1; fi
echo "BUILD OK"
export LD_LIBRARY_PATH=$BUILD/modules/nei:$BUILD/modules/neixx:$BUILD/3rdparty
export ASAN_OPTIONS=detect_leaks=0:halt_on_error=0
echo "=== 2. Full suite (excl HostResolver env) ==="
timeout 900 "$BUILD/tests/nei_tests" --gtest_filter="-HostResolverTest.*" > "$LOG" 2>&1
rc=$?
echo "exit=$rc"
grep -E "\[  PASSED  \]|\[  FAILED  \]|FAILED TEST|ERROR: AddressSanitizer|SUMMARY: AddressSanitizer" "$LOG" | tail -10
echo "=== 3. Any ASAN errors ==="
grep -c "ERROR: AddressSanitizer" "$LOG" || true
