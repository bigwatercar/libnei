#!/usr/bin/env bash
cd /mnt/c/Personal/Projects/LibNei/libnei-src/build/linux-gcc-release-shared

TEST_NAME="AsyncFilePosixStressTest.CloseRaceCancelsInFlightOperationsWithoutHang"
PASS=0
FAIL=0
FAILED_ITERS=""

i=1
while [ $i -le 30 ]; do
  echo "=== RUN $i/30 ==="
  ctest -R "^${TEST_NAME}$" --output-on-failure
  code=$?
  if [ $code -eq 0 ]; then
    PASS=$((PASS+1))
    echo "RESULT: PASS"
  else
    FAIL=$((FAIL+1))
    FAILED_ITERS="$FAILED_ITERS $i"
    echo "RESULT: FAIL exit=$code"
  fi
  echo
  i=$((i+1))
done

echo "SUMMARY PASS=$PASS FAIL=$FAIL FAILED_ITERS:$FAILED_ITERS"
