#!/usr/bin/env bash
#
# Unified benchmark runner for all libnei benches (POSIX / WSL).
#
# Reads the same bench/manifest as the Windows runner (benches.tsv), so both
# platforms measure the same set with the same arguments and round counts, and
# writes the same log layout (benches/<name>.log with "=== <name> ===" headers).
#
# Usage:
#   ./run_all_benches.sh --bin-dir <bench-dir> --log-dir <results-dir> [--only n1,n2] [--manifest <file>]
#
# Example (WSL, release build):
#   ./bench/run_all_benches.sh \
#       --bin-dir ~/libnei_build/wsl-release-shared/bench \
#       --log-dir bench/results
#
# The bench binaries need to find libnei.so (and its deps), so run this from the
# build root or export LD_LIBRARY_PATH as the build layout requires; the runner
# prepends --bin-dir and its parent/lib to LD_LIBRARY_PATH automatically.

set -u

usage() {
  sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
}

BIN_DIR=""
LOG_DIR=""
ONLY=""
MANIFEST=""

while [ $# -gt 0 ]; do
  case "$1" in
    --bin-dir)  BIN_DIR="${2:-}"; shift 2 ;;
    --log-dir)  LOG_DIR="${2:-}"; shift 2 ;;
    --only)     ONLY="${2:-}"; shift 2 ;;
    --manifest) MANIFEST="${2:-}"; shift 2 ;;
    -h|--help)  usage ;;
    *) echo "unknown argument: $1" >&2; usage ;;
  esac
done

[ -n "$BIN_DIR" ] || usage
[ -n "$LOG_DIR" ] || usage

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -n "$MANIFEST" ] || MANIFEST="$SCRIPT_DIR/benches.tsv"
[ -f "$MANIFEST" ] || { echo "manifest not found: $MANIFEST" >&2; exit 1; }

BIN_DIR="$(cd "$BIN_DIR" 2>/dev/null && pwd)" || { echo "bin dir not found" >&2; exit 1; }

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="$LOG_DIR/bench_$TIMESTAMP"
mkdir -p "$OUT_DIR"
LOG_TMP="$OUT_DIR/log_temp"
mkdir -p "$LOG_TMP"

# Shared-library search path: the bench dir plus the build root next to it.
BUILD_ROOT="$(cd "$BIN_DIR/.." && pwd)"
export LD_LIBRARY_PATH="$BIN_DIR:$BUILD_ROOT/lib:$BUILD_ROOT/_deps/spdlog-build:$BUILD_ROOT${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

echo "=== libnei Unified Bench Runner ==="
echo "Bin dir : $BIN_DIR"
echo "Log dir : $OUT_DIR"
echo "Manifest: $MANIFEST"
echo "Started : $(date '+%Y-%m-%d %H:%M:%S')"
echo ""

selected() {
  [ -z "$ONLY" ] && return 0
  case ",$ONLY," in *",$1,"*) return 0 ;; *) return 1 ;; esac
}

while IFS='|' read -r name exe rounds args; do
  # The manifest may be checked out with CRLF on Windows; strip CR from every field.
  name="${name%$'\r'}"; exe="${exe%$'\r'}"; rounds="${rounds%$'\r'}"; args="${args%$'\r'}"
  case "$name" in ''|\#*) continue ;; esac
  selected "$name" || continue

  exe_path="$BIN_DIR/$exe"
  if [ ! -x "$exe_path" ]; then
    echo "  [SKIP] $exe not found"
    continue
  fi

  # {tmp} -> per-run scratch dir, so log benches write into the run directory.
  args_expanded="${args//\{tmp\}/$LOG_TMP}"
  log_file="$OUT_DIR/$name.log"

  r=1
  while [ "$r" -le "$rounds" ]; do
    if [ "$rounds" -gt 1 ]; then
      echo "  [$name] round $r/$rounds"
      printf '\n=== %s === Round %d / %d ===\n\n' "$name" "$r" "$rounds" >> "$log_file"
    else
      echo "  [$name] round 1/1"
      printf '\n=== %s ===\n\n' "$name" >> "$log_file"
    fi
    if [ -n "$args_expanded" ]; then
      # shellcheck disable=SC2086  # args are intentionally word-split
      "$exe_path" $args_expanded >> "$log_file" 2>&1
    else
      "$exe_path" >> "$log_file" 2>&1
    fi
    r=$((r + 1))
  done
done < "$MANIFEST"

rm -rf "$LOG_TMP"

echo ""
echo "=== Benchmarks complete ==="
echo "Finished: $(date '+%Y-%m-%d %H:%M:%S')"
echo "Logs   : $OUT_DIR"
find "$OUT_DIR" -maxdepth 1 -name '*.log' -printf '  %8s  %f\n' 2>/dev/null | sort -k2

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
REPORT="$OUT_DIR/report.md"

task_table() {
  local file="$1"
  [ -f "$file" ] || return 0
  awk '
    /^--- / { sc = $0; sub(/^--- /, "", sc); sub(/ ---$/, "", sc); next }
    /^Post throughput:/ { v = $3 + 0; n[sc]++; s[sc] += v; q[sc] += v * v
                          if (!(sc in mn) || v < mn[sc]) mn[sc] = v
                          if (v > mx[sc]) mx[sc] = v }
    END {
      for (k in n) {
        m = s[k] / n[k]; d = (n[k] > 1) ? sqrt(q[k] / n[k] - m * m) : 0
        printf "| %s | %d | %.0f /s | %.0f /s | %.0f /s | %.0f /s |\n", k, n[k], m, d, mn[k], mx[k]
      }
    }' "$file" | sort
}

log_table() {
  local file="$1"
  [ -f "$file" ] || return 0
  awk '
    /^[A-Za-z].*:$/ && !/^(Iterations|Total|Average|Logs|Runtime|Phase|Output|Enqueue|E2E|Flush)/ {
      sc = $0; sub(/:$/, "", sc); sub(/ \(File:.*\)$/, "", sc); next }
    /^ +E2E avg per log:/ { a[sc] += $5; na[sc]++; next }
    /^ +E2E logs\/sec:/ { l[sc] += $3; nl[sc]++; next }
    END {
      for (k in na) {
        m = a[k] / na[k]; ml = l[k] / nl[k]
        printf "| %s | %d | %.3f | %.0f |\n", k, na[k], m, ml
      }
    }' "$file" | sort
}

{
  echo "# libnei Benchmark Report"
  echo ""
  echo "**Date**: $(date '+%Y-%m-%d %H:%M:%S')  "
  echo "**Host**: $(uname -srm)  "
  echo "**Source**: $OUT_DIR  "
  echo ""

  idx=0
  for pair in "1. Task Thread (tracing ON):task_thread_tracing_on" \
              "2. Task Thread (tracing OFF):task_thread_tracing_off" \
              "3. Task ThreadPool:task_threadpool" \
              "4. Task ThreadPool Parallel:task_threadpool_parallel"; do
    idx=$((idx + 1))
    title="${pair%%:*}"
    name="${pair##*:}"
    file="$OUT_DIR/$name.log"
    [ -f "$file" ] || continue
    echo "## $title"
    echo ""
    echo "| Scenario | Runs | Mean Post | Stddev | Min | Max |"
    echo "|---|---:|---:|---:|---:|---:|"
    task_table "$file"
    echo ""
  done

  if [ -f "$OUT_DIR/log_bench.log" ]; then
    echo "## 5. Log Bench (NEI)"
    echo ""
    echo "| Benchmark | Runs | Avg E2E μs/log | Avg E2E logs/s |"
    echo "|---|---:|---:|---:|"
    log_table "$OUT_DIR/log_bench.log"
    echo ""
  fi

  if [ -f "$OUT_DIR/log_bench_compare.log" ]; then
    echo "## 6. Log Compare (NEI vs spdlog)"
    echo ""
    awk '
      /^--- / { sec = $0; sub(/^--- /, "", sec); sub(/ ---$/, "", sec); cur = ""; next }
      /^\[/ {
        lib = $0; sub(/^\[/, "", lib); sub(/\].*/, "", lib)
        name = $0; sub(/^\[[^]]*\][ ]*/, "", name)
        key = sec SUBSEP lib SUBSEP name
        if (!(key in first)) { first[key] = ++n; order[n] = key }
        cur = key; next
      }
      /^ +Average time per log:/ { if (cur != "" && $5 + 0 > 0) { a[cur] += $5; na[cur]++ } ; next }
      /^ +Logs per second:/ { if (cur != "" && $4 + 0 > 0) { l[cur] += $4 }; next }
      END {
        lastsec = ""
        for (i = 1; i <= n; i++) {
          k = order[i]; split(k, p, SUBSEP)
          if (!(k in na)) continue
          if (p[1] != lastsec) {
            printf "\n**%s**\n\n| Library | Benchmark | Avg μs/log | logs/s |\n|---|---|---:|---:|\n", p[1]
            lastsec = p[1]
          }
          printf "| %s | %s | %.3f | %.0f |\n", p[2], p[3], a[k] / na[k], l[k] / na[k]
        }
      }' "$OUT_DIR/log_bench_compare.log"
    echo ""
  fi

  echo "## 7. Single-run Benches"
  echo ""
  for pair in "PostJob:post_job" "Flake ID:flake_id" "String Append:string_append" \
              "Callback:callback" "AsyncFile:async_file" "PipeStream:pipe_stream" \
              "PipeStream Cross-Process:pipe_stream_cross_process" "TCP Loopback:tcp_loopback" \
              "TCP RTT:tcp_rtt" "TCP Throughput:tcp_throughput" "TCP Conn Stress:tcp_conn_stress" \
              "TLS Throughput:tls_throughput" "HTTP/1.1 Throughput:http_throughput" \
              "HTTP/2 seq:http2_throughput_seq" "HTTP/2 par8:http2_throughput_par8" \
              "HTTP/2 par64:http2_throughput_par64" "Parallel Runner:parallel_runner" \
              "Task Priority Perf:task_priority_perf"; do
    title="${pair%%:*}"
    name="${pair##*:}"
    file="$OUT_DIR/$name.log"
    [ -f "$file" ] || continue
    echo "### $title"
    echo ""
    echo '```'
    tail -n +4 "$file"
    echo '```'
    echo ""
  done
} > "$REPORT"

echo ""
echo "Report : $REPORT"
echo ""
echo "=== All done ==="
