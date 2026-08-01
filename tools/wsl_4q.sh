#!/bin/bash
# WSL 4-quadrant build & test
SRC="/mnt/c/Personal/Projects/LibNei/libnei-src"
LOG="${SRC}/build/wsl_full_result.txt"
NPROC=$(nproc)

echo "=== WSL 4-Quadrant Build & Test ===" > "$LOG"
echo "Started: $(date)" >> "$LOG"
echo "" >> "$LOG"

build_and_test() {
    local name="$1"
    local build_type="$2"
    local shared="$3"
    local dir="${SRC}/build/wsl-${name}"

    echo "" >> "$LOG"
    echo "========== ${name} (${build_type} / ${shared}) ==========" >> "$LOG"
    echo "[${name}] Configuring..." | tee -a "$LOG"

    cmake -S "$SRC" -B "$dir" -G Ninja \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DBUILD_SHARED_LIBS="$shared" \
        -DNEI_BUILD_TESTS=ON >> "$LOG" 2>&1
    if [ $? -ne 0 ]; then
        echo "FAIL: ${name} CONFIGURE" | tee -a "$LOG"
        return 1
    fi

    echo "[${name}] Building..." | tee -a "$LOG"
    cmake --build "$dir" --config "$build_type" -j "$NPROC" >> "$LOG" 2>&1
    if [ $? -ne 0 ]; then
        echo "FAIL: ${name} BUILD" | tee -a "$LOG"
        return 1
    fi

    echo "[${name}] Testing..." | tee -a "$LOG"
    cd "$dir"
    ctest --output-on-failure -C "$build_type" >> "$LOG" 2>&1
    local test_rc=$?
    cd "$SRC"

    if [ $test_rc -eq 0 ]; then
        echo "PASS: ${name}" | tee -a "$LOG"
    else
        local nfail=$(grep -c "tests failed" "$LOG" | tail -1)
        echo "FAIL: ${name} TESTS (${nfail} failures)" | tee -a "$LOG"
    fi
}

# Q1: debug-shared (already configured & passed)
echo "[Q1] debug-shared already built, skipping configure/build..." | tee -a "$LOG"
cd "${SRC}/build/wsl-debug-shared"
ctest --output-on-failure -C Debug >> "$LOG" 2>&1
rc=$?
cd "$SRC"
if [ $rc -eq 0 ]; then
    echo "PASS: wsl-debug-shared" | tee -a "$LOG"
else
    echo "FAIL: wsl-debug-shared TESTS" | tee -a "$LOG"
fi

# Q2-4
build_and_test "debug-static" "Debug" "OFF"
build_and_test "rel-shared" "Release" "ON"
build_and_test "rel-static" "Release" "OFF"

echo "" >> "$LOG"
echo "=== Done: $(date) ===" >> "$LOG"
echo "" >> "$LOG"
echo "=== Summary ===" >> "$LOG"
grep -E "^PASS:|^FAIL:" "$LOG" >> "$LOG"
echo "" >> "$LOG"
echo "Full log: $LOG"
