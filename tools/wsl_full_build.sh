#!/bin/bash
# wsl_full_build.sh — 4-quadrant WSL build + test
SRC=/mnt/c/Personal/Projects/LibNei/libnei-src
BUILD_BASE=/mnt/c/Personal/Projects/LibNei/libnei-src/build
RESULTS=$BUILD_BASE/wsl_results.txt
rm -rf "$BUILD_BASE/wsl-"*
echo "=== WSL 4-Quadrant Build & Test ===" > "$RESULTS"
echo "Started: $(date)" >> "$RESULTS"
echo "" >> "$RESULTS"

build_and_test() {
    local name=$1
    local build_type=$2
    local shared=$3
    local dir="$BUILD_BASE/wsl-$name"
    echo "" >> "$RESULTS"
    echo "========== $name ($build_type / $([ "$shared" = "ON" ] && echo "shared" || echo "static")) ==========" >> "$RESULTS"
    echo "  Configuring..." >> "$RESULTS"
    cmake -S "$SRC" -B "$dir" \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DBUILD_SHARED_LIBS="$shared" \
        -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
        >> "$RESULTS" 2>&1
    if [ $? -ne 0 ]; then
        echo "  CONFIGURE FAILED" >> "$RESULTS"
        echo "FAIL:$name" >> /tmp/wsl_results_summary.txt
        return 1
    fi
    echo "  Building..." >> "$RESULTS"
    cmake --build "$dir" --config "$build_type" -j$(nproc) >> "$RESULTS" 2>&1
    local build_rc=$?
    if [ $build_rc -ne 0 ]; then
        echo "  BUILD FAILED (rc=$build_rc)" >> "$RESULTS"
        echo "FAIL:$name" >> /tmp/wsl_results_summary.txt
        return 1
    fi
    echo "  Testing..." >> "$RESULTS"
    cd "$dir"
    timeout 300 ctest --output-on-failure -C "$build_type" >> "$RESULTS" 2>&1
    local test_rc=$?
    local passed=$(grep -c "tests passed" "$RESULTS" | tail -1 || echo "?")
    local failed=$(grep -c "tests failed" "$RESULTS" | tail -1 || echo "?")
    echo "  TEST RESULT: $passed passed, $failed failed" >> "$RESULTS"
    if [ $test_rc -eq 0 ]; then
        echo "PASS:$name" >> /tmp/wsl_results_summary.txt
    else
        echo "FAIL:$name" >> /tmp/wsl_results_summary.txt
    fi
    return $test_rc
}

# Initialize summary
echo "" > /tmp/wsl_results_summary.txt

# 4 quadrants: Debug-{shared,static}, Release-{shared,static}
build_and_test "debug-shared"  Debug    ON
build_and_test "debug-static"  Debug    OFF
build_and_test "rel-shared"    Release  ON
build_and_test "rel-static"    Release  OFF

# Summary
echo "" >> "$RESULTS"
echo "========== SUMMARY ==========" >> "$RESULTS"
cat /tmp/wsl_results_summary.txt >> "$RESULTS"
echo "" >> "$RESULTS"
echo "Finished: $(date)" >> "$RESULTS"
echo "Results: $RESULTS"
cat /tmp/wsl_results_summary.txt
