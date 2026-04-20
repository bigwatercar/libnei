#!/usr/bin/env bash

set -euo pipefail

# ThreadPool benchmark (Linux/WSL) - rebuild + run + summarize
# Usage:
#   ./bench/rebuild_and_bench_threadpool.sh
#   ./bench/rebuild_and_bench_threadpool.sh --skip-build --runs-10k 5 --runs-100k 3

RUNS_10K=3
RUNS_100K=2
SKIP_BUILD=0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/linux-ninja-release-shared"
OUT_MD="$REPO_ROOT/threadpool_bench_latest.md"

SCENARIOS=(
  "1 thread"
  "2 threads"
  "4 threads"
  "default"
  "default_no_comp"
  "1 thread_noop"
  "2 threads_noop"
  "4 threads_noop"
  "default_noop"
  "default_no_comp_noop"
  "default_delayed_mix"
  "default_no_comp_delayed_mix"
  "default_delayed_fixed"
  "default_no_comp_delayed_fixed"
)

usage() {
  cat <<'EOF'
Usage: rebuild_and_bench_threadpool.sh [options]

Options:
  --runs-10k N      Repeat count for 10,000-task suite (default: 3)
  --runs-100k N     Repeat count for 100,000-task suite (default: 2)
  --skip-build      Skip cmake --build
  --repo-root PATH  Repository root (default: script_dir/..)
  --build-dir PATH  Build directory (default: <repo>/build/linux-ninja-release-shared)
  --out-md PATH     Markdown output path (default: <repo>/threadpool_bench_latest.md)
  -h, --help        Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --runs-10k)
      RUNS_10K="$2"
      shift 2
      ;;
    --runs-100k)
      RUNS_100K="$2"
      shift 2
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    --repo-root)
      REPO_ROOT="$2"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --out-md)
      OUT_MD="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

EXE_PATH="$BUILD_DIR/bench/task_threadpool_bench"

if [[ ! -d "$BUILD_DIR" ]]; then
  echo "Build directory not found: $BUILD_DIR" >&2
  echo "Tip: pass --build-dir to match your WSL preset output." >&2
  exit 1
fi

if [[ "$SKIP_BUILD" -eq 0 ]]; then
  echo "==> Building..."
  cmake --build "$BUILD_DIR" --target neixx task_threadpool_bench
  echo
fi

if [[ ! -x "$EXE_PATH" ]]; then
  echo "Benchmark executable not found: $EXE_PATH" >&2
  exit 1
fi

pkill -f task_threadpool_bench >/dev/null 2>&1 || true

raw_tsv="$(mktemp)"
summary_tmp="$(mktemp)"
summary_tsv="$(mktemp)"
trap 'rm -f "$raw_tsv" "$summary_tmp" "$summary_tsv"' EXIT

scenario_csv="$(IFS=,; echo "${SCENARIOS[*]}")"
scenario_filter="|${scenario_csv//,/|}|"

line_re='^(.+) \| workers=([0-9]+) \| enqueue_only_ms=([0-9.]+) \| drain_wait_ms=([0-9.]+) \| total_ms=([0-9.]+) \| enqueue_only_ns_per_task=[0-9.]+ \| drain_wait_ns_per_task=[0-9.]+ \| total_ns_per_task=([0-9.]+) \| sum=[0-9]+ \| status=(PASS|FAIL)$'

run_suite() {
  local task_count="$1"
  local runs="$2"

  for ((run=1; run<=runs; run++)); do
    printf '  Running %6d tasks  (run %d/%d)\n' "$task_count" "$run" "$runs"
    while IFS= read -r line; do
      if [[ "$line" =~ $line_re ]]; then
        local scenario="${BASH_REMATCH[1]}"
        local workers="${BASH_REMATCH[2]}"
        local enq_ms="${BASH_REMATCH[3]}"
        local drn_ms="${BASH_REMATCH[4]}"
        local tot_ms="${BASH_REMATCH[5]}"
        local tot_ns="${BASH_REMATCH[6]}"
        local status="${BASH_REMATCH[7]}"

        if [[ "$scenario_filter" == *"|$scenario|"* ]]; then
          printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$task_count" "$run" "$scenario" "$workers" \
            "$enq_ms" "$drn_ms" "$tot_ms" "$tot_ns" "$status" >> "$raw_tsv"
        fi
      fi
    done < <("$EXE_PATH" "$task_count")
  done
}

run_suite 10000 "$RUNS_10K"
run_suite 100000 "$RUNS_100K"

awk -F'\t' -v OFS='\t' -v scenario_csv="$scenario_csv" '
BEGIN {
  split(scenario_csv, scs, ",")
  for (i = 1; i <= length(scs); i++) rank[scs[i]] = i
}
{
  n = $1 + 0
  sc = $3
  key = n SUBSEP sc
  workers[key] = $4 + 0
  enq = $5 + 0.0
  drn = $6 + 0.0
  tot = $7 + 0.0
  tns = $8 + 0.0
  st = $9

  cnt[key]++
  sum_enq[key] += enq
  sum_drn[key] += drn
  sum_tot[key] += tot
  sum_tns[key] += tns

  tps = int((n * 1000.0) / tot)
  sum_tps[key] += tps
  if (st == "PASS") pass_cnt[key]++

  delta = tot - mean[key]
  mean[key] += delta / cnt[key]
  m2[key] += delta * (tot - mean[key])
}
END {
  for (key in cnt) {
    split(key, parts, SUBSEP)
    n = parts[1]
    sc = parts[2]
    c = cnt[key]
    stddev = (c > 1) ? sqrt(m2[key] / c) : 0.0
    print n, sc, workers[key], c,
          sum_enq[key] / c,
          sum_drn[key] / c,
          sum_tot[key] / c,
          stddev,
          sum_tns[key] / c,
          int(sum_tps[key] / c),
          pass_cnt[key] + 0,
          rank[sc] + 0
  }
}
' "$raw_tsv" > "$summary_tmp"

sort -t $'\t' -k1,1n -k12,12n "$summary_tmp" | cut -f1-11 > "$summary_tsv"

echo
printf 'ThreadPool Benchmark  -  %s\n\n' "$(date '+%Y-%m-%d %H:%M:%S')"

for n in 10000 100000; do
  label="10 000"
  [[ "$n" -eq 100000 ]] && label="100 000"

  printf -- '-- %s tasks ----------------------------------------------------------------------\n' "$label"
  printf '%-32s  %4s  %7s  %7s  %7s  %7s  %9s  %10s  %5s\n' \
    "Scenario" "W" "Enq ms" "Drn ms" "Tot ms" "Stddev" "ns/task" "Avg TPS" "PASS"
  printf '%-32s  %4s  %7s  %7s  %7s  %7s  %9s  %10s  %5s\n' \
    "--------------------------------" "----" "-------" "-------" "-------" "-------" "---------" "----------" "-----"

  awk -F'\t' -v n="$n" '
  $1 == n {
    pass = $11 "/" $4
    printf "%-32s  %4d  %7.2f  %7.2f  %7.2f  %7.2f  %9.1f  %10d  %5s\n",
      $2, $3, $5, $6, $7, $8, $9, $10, pass
  }
  ' "$summary_tsv"
  echo
done

{
  echo "# ThreadPool Benchmark"
  echo
  echo "Generated: $(date '+%Y-%m-%d %H:%M:%S')"
  echo "Runs: 10k x $RUNS_10K, 100k x $RUNS_100K"
  echo

  for n in 10000 100000; do
    echo "## $n tasks"
    echo
    echo "| Scenario | W | Enq ms | Drn ms | Tot ms | Stddev | ns/task | Avg TPS | PASS |"
    echo "|---|---:|---:|---:|---:|---:|---:|---:|---:|"
    awk -F'\t' -v n="$n" '
    $1 == n {
      pass = $11 "/" $4
      printf "| %s | %d | %.2f | %.2f | %.2f | %.2f | %.1f | %d | %s |\n",
        $2, $3, $5, $6, $7, $8, $9, $10, pass
    }
    ' "$summary_tsv"
    echo
  done
} > "$OUT_MD"

echo "Saved: $OUT_MD"
