#!/bin/bash
SRC=/mnt/c/Personal/Projects/LibNei/libnei-src
BUILD=$SRC/build/linux-gcc-release-shared
BIN=$BUILD/tests/nei_tests
export LD_LIBRARY_PATH=$BUILD
"$BIN" --gtest_list_tests 2>/dev/null > /tmp/wsl_tests.txt
echo "total lines: $(wc -l < /tmp/wsl_tests.txt)"
echo "suites: $(grep -cE '^[^ ]' /tmp/wsl_tests.txt)"
