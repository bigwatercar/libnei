#!/bin/bash
# WSL full suite WITHOUT any filter - let DNS/env failures surface.
SRC=/mnt/c/Personal/Projects/LibNei/libnei-src
BUILD=$SRC/build/linux-gcc-release-shared
BIN=$BUILD/tests/nei_tests
export LD_LIBRARY_PATH=$BUILD/modules/nei:$BUILD/modules/neixx:$BUILD/3rdparty
"$BIN" 2>&1 | tee /tmp/wsl_full.txt | grep -E "\[  PASSED  \]|\[  FAILED  \]|FAILED TEST|tests from .* test suites ran|SKIPPED"
