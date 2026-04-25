#!/usr/bin/env bash

set -euo pipefail

# Log benchmark (Linux) - rebuild + run + summarize
# Usage:
#   ./bench/rebuild_and_bench_log.sh
#   ./bench/rebuild_and_bench_log.sh --runs 10 --skip-build
#   ./bench/rebuild_and_bench_log.sh --build-dir /path/to/build

RUNS=5
SKIP_BUILD=0
BUILD_DIR=""
OUTPUT_DIR=""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_MD="$REPO_ROOT/log_bench_latest.md"

usage() {
  cat <<'EOF'
Usage: rebuild_and_bench_log.sh [options]

Options:
  --runs N         Number of runs (default: 5)
  --skip-build     Skip cmake --build
  --build-dir PATH Build directory override
  --output-dir PATH Log output directory (default: <repo>/bench/output)
  --out-md PATH    Markdown output path (default: <repo>/log_bench_latest.md)
  -h, --help       Show help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --runs)
      RUNS="$2"
      shift 2
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --output-dir)
      OUTPUT_DIR="$2"
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

if [[ -z "$BUILD_DIR" ]]; then
  for d in \
    "$REPO_ROOT/build-linux" \
    "$REPO_ROOT/build/linux-gcc-release-shared" \
    "$REPO_ROOT/build/linux-gcc-debug-shared" \
    "$REPO_ROOT/build/linux-make-debug-shared"; do
    if [[ -x "$d/bench/log_bench" ]]; then
      BUILD_DIR="$d"
      break
    fi
  done
fi

if [[ -z "$BUILD_DIR" ]]; then
  BUILD_DIR="$REPO_ROOT/build-linux"
fi

EXE_PATH="$BUILD_DIR/bench/log_bench"

if [[ -z "$OUTPUT_DIR" ]]; then
  OUTPUT_DIR="$REPO_ROOT/bench/output"
fi

mkdir -p "$OUTPUT_DIR"

LOG_FILES=(
  "$OUTPUT_DIR/log_bench_info.log"
  "$OUTPUT_DIR/log_bench_warn.log"
  "$OUTPUT_DIR/log_bench_error.log"
  "$OUTPUT_DIR/log_bench_format.log"
  "$OUTPUT_DIR/log_bench_verbose.log"
  "$OUTPUT_DIR/log_bench_info_literal.log"
  "$OUTPUT_DIR/log_bench_verbose_literal.log"
)

if [[ ! -d "$BUILD_DIR" ]]; then
  echo "Build directory not found: $BUILD_DIR" >&2
  exit 1
fi

pkill -f log_bench >/dev/null 2>&1 || true
pkill -f nei_tests >/dev/null 2>&1 || true

if [[ "$SKIP_BUILD" -eq 0 ]]; then
  echo "==> Building..."
  cmake --build "$BUILD_DIR" --target nei neixx log_bench
  echo
fi

if [[ ! -x "$EXE_PATH" ]]; then
  echo "Executable not found: $EXE_PATH" >&2
  exit 1
fi

raw_tsv="$(mktemp)"
summary_tsv="$(mktemp)"
trap 'rm -f "$raw_tsv" "$summary_tsv"' EXIT

parse_run() {
  local run_idx="$1"
  local run_out="$2"
  awk -v run="$run_idx" '
  function reset_cur() {
    iterations=""; enqueue_total=""; enqueue_avg=""; flush_total="";
    e2e_total=""; e2e_avg=""; enqueue_lps=""; e2e_lps="";
    prod=""; flush_wait=""; wakeups=""; hwm=""; file_size="";
  }
  function emit_cur() {
    if (bench != "" && e2e_lps != "") {
      print run "\t" bench "\t" iterations "\t" enqueue_total "\t" enqueue_avg "\t" flush_total "\t" e2e_total "\t" e2e_avg "\t" enqueue_lps "\t" e2e_lps "\t" prod "\t" flush_wait "\t" wakeups "\t" hwm "\t" file_size;
      bench="";
      reset_cur();
    }
  }
  BEGIN { bench=""; reset_cur(); }
  {
    line=$0;

    if (line ~ /:$/ && line !~ /^[[:space:]]/ && line !~ /^=+$/) {
      emit_cur();
      bench=line;
      sub(/:$/, "", bench);
      sub(/ \(File: .*\)$/, "", bench);
      reset_cur();
      next;
    }

    if (bench == "") next;

    if (match(line, /^[[:space:]]+Iterations:[[:space:]]+([0-9]+)/, m)) { iterations=m[1]; next; }
    if (match(line, /^[[:space:]]+Enqueue total:[[:space:]]+([0-9.]+) microseconds$/, m)) { enqueue_total=m[1]; next; }
    if (match(line, /^[[:space:]]+Enqueue avg per log:[[:space:]]+([0-9.]+) microseconds$/, m)) { enqueue_avg=m[1]; next; }
    if (match(line, /^[[:space:]]+Flush total:[[:space:]]+([0-9.]+) microseconds$/, m)) { flush_total=m[1]; next; }
    if (match(line, /^[[:space:]]+E2E total:[[:space:]]+([0-9.]+) microseconds$/, m)) { e2e_total=m[1]; next; }
    if (match(line, /^[[:space:]]+E2E avg per log:[[:space:]]+([0-9.]+) microseconds$/, m)) { e2e_avg=m[1]; next; }
    if (match(line, /^[[:space:]]+Enqueue logs\/sec:[[:space:]]+([0-9.eE+-]+)/, m)) { enqueue_lps=m[1]; next; }
    if (match(line, /^[[:space:]]+E2E logs\/sec:[[:space:]]+([0-9.eE+-]+)/, m)) { e2e_lps=m[1]; next; }
    if (match(line, /^[[:space:]]+Total time:[[:space:]]+([0-9.]+) microseconds$/, m)) { e2e_total=m[1]; next; }
    if (match(line, /^[[:space:]]+Average time per log:[[:space:]]+([0-9.]+) microseconds$/, m)) { e2e_avg=m[1]; next; }
    if (match(line, /^[[:space:]]+Logs per second:[[:space:]]+([0-9.eE+-]+)/, m)) { e2e_lps=m[1]; next; }
    if (match(line, /^[[:space:]]+Runtime stats:[[:space:]]+producer_spins=([0-9]+),[[:space:]]+flush_wait_loops=([0-9]+),[[:space:]]+consumer_wakeups=([0-9]+),[[:space:]]+ring_hwm=([0-9]+)/, m)) {
      prod=m[1]; flush_wait=m[2]; wakeups=m[3]; hwm=m[4]; next;
    }
    if (match(line, /^[[:space:]]+File size:[[:space:]]+(-?[0-9]+) bytes$/, m)) {
      file_size=m[1];
      emit_cur();
      next;
    }
    if (line ~ /^[[:space:]]*$/) {
      emit_cur();
      next;
    }
  }
  END {
    emit_cur();
  }
  ' "$run_out" >> "$raw_tsv"
}

for ((r=1; r<=RUNS; r++)); do
  for f in "${LOG_FILES[@]}"; do
    if [[ -f "$f" ]]; then
      rm -f "$f"
      echo "  Deleted: $f"
    fi
  done

  echo "  Running log_bench (run $r/$RUNS)"
  run_out="$(mktemp)"
  "$EXE_PATH" "$OUTPUT_DIR" > "$run_out"
  parse_run "$r" "$run_out"
  rm -f "$run_out"
done

ordered_csv='Log Info,Log Warn,Log Error,Log with Formatting,Log Info (literal),Log Verbose,Log Verbose (literal),File Log Info,File Log Warn,File Log Error,File Log with Formatting,File Log Verbose,File Log Info (literal),File Log Verbose (literal)'

awk -F'\t' -v OFS='\t' -v ordered_csv="$ordered_csv" '
BEGIN {
  split(ordered_csv, arr, ",")
  for (i = 1; i <= length(arr); i++) rank[arr[i]] = i
}
{
  bench=$2
  key=bench
  c[key]++
  iter[key]=$3
  s_enq_total[key]+=($4+0)
  s_enq_avg[key]+=($5+0)
  if ($6 != "") { s_flush_total[key]+=($6+0); c_flush[key]++ }
  s_e2e_total[key]+=($7+0)
  s_e2e_avg[key]+=($8+0)
  s_enq_lps[key]+=($9+0)
  s_e2e_lps[key]+=($10+0)
  s_prod[key]+=($11+0)
  s_flush_wait[key]+=($12+0)
  s_wakeups[key]+=($13+0)
  s_hwm[key]+=($14+0)
  if ($15 != "") { s_file[key]+=($15+0); c_file[key]++ }

  delta=($8+0)-mean[key]
  mean[key]+=delta/c[key]
  m2[key]+=delta*(($8+0)-mean[key])
}
END {
  for (k in c) {
    std=(c[k] > 1) ? sqrt(m2[k]/c[k]) : 0
    flush=(c_flush[k] > 0) ? s_flush_total[k]/c_flush[k] : -1
    fs=(c_file[k] > 0) ? s_file[k]/c_file[k] : -1
    mode=(k ~ /^File /) ? "file" : "memory"
    print k, c[k], iter[k], s_enq_total[k]/c[k], s_enq_avg[k]/c[k], flush,
          s_e2e_total[k]/c[k], s_e2e_avg[k]/c[k], std,
          s_enq_lps[k]/c[k], s_e2e_lps[k]/c[k], fs,
          s_prod[k]/c[k], s_flush_wait[k]/c[k], s_wakeups[k]/c[k], s_hwm[k]/c[k],
          mode, rank[k]+0
  }
}
' "$raw_tsv" | sort -t $'\t' -k18,18n > "$summary_tsv"

echo
printf 'Log Benchmark  -  %s  (Runs: %d)\n\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$RUNS"
printf '%-28s  %4s  %12s  %12s  %10s  %12s  %12s\n' \
  'Benchmark' 'Runs' 'Enq us/log' 'E2E us/log' 'Stddev' 'E2E logs/s' 'File bytes'
printf '%-28s  %4s  %12s  %12s  %10s  %12s  %12s\n' \
  '----------------------------' '----' '------------' '------------' '----------' '------------' '------------'
awk -F'\t' '
{
  fs = ($12 >= 0) ? sprintf("%.0f", $12) : "-"
  printf "%-28s  %4d  %12.4f  %12.4f  %10.4f  %12.0f  %12s\n", $1, $2, $5, $8, $9, $11, fs
}
' "$summary_tsv"

echo
echo 'Diagnostics (avg per benchmark)'
echo
printf '%-28s  %12s  %12s  %12s  %10s\n' \
  'Benchmark' 'Prod spins' 'Flush waits' 'Wakeups' 'Ring HWM'
printf '%-28s  %12s  %12s  %12s  %10s\n' \
  '----------------------------' '------------' '------------' '------------' '----------'
awk -F'\t' '{
  printf "%-28s  %12.0f  %12.2f  %12.2f  %10.2f\n", $1, $13, $14, $15, $16
}' "$summary_tsv"

best_line="$(awk -F'\t' 'NR==1 || $8 < min { min=$8; line=$0 } END{ print line }' "$summary_tsv")"
worst_line="$(awk -F'\t' 'NR==1 || $8 > max { max=$8; line=$0 } END{ print line }' "$summary_tsv")"

{
  echo '# Log Benchmark'
  echo
  echo "Generated: $(date '+%Y-%m-%d %H:%M:%S')"
  echo "Runs: $RUNS"
  echo 'Build: Release'
  echo

  echo '## Highlights'
  echo
  echo '| Type | Benchmark | Mode | Avg e2e us/log | Avg e2e logs/sec |'
  echo '|---|---|---|---:|---:|'
  awk -F'\t' -v row="$best_line" 'BEGIN{ split(row,f,"\t"); printf "| Best | %s | %s | %.4f | %.0f |\n", f[1], f[17], f[8], f[11] }'
  awk -F'\t' -v row="$worst_line" 'BEGIN{ split(row,f,"\t"); printf "| Worst | %s | %s | %.4f | %.0f |\n", f[1], f[17], f[8], f[11] }'
  echo

  echo '## Memory vs File Ratios'
  echo
  echo '| Scenario | Memory avg e2e us/log | File avg e2e us/log | File/Memory x |'
  echo '|---|---:|---:|---:|'
  awk -F'\t' '
  BEGIN{}
  {
    if ($17 == "memory") {
      sc=$1; sub(/^Log /, "", sc)
      mem[sc]=$8
    } else {
      sc=$1; sub(/^File Log /, "", sc)
      file[sc]=$8
    }
  }
  END {
    for (s in mem) {
      if (s in file && mem[s] > 0) {
        printf "| %s | %.4f | %.4f | %.2f |\n", s, mem[s], file[s], file[s]/mem[s]
      }
    }
  }' "$summary_tsv" | sort
  echo

  echo '## In-memory sink'
  echo
  echo '| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |'
  echo '|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|'
  awk -F'\t' '$17=="memory" {
    flush = ($6 >= 0) ? sprintf("%.2f", $6) : "-"
    fs = ($12 >= 0) ? sprintf("%.0f", $12) : "-"
    printf "| %s | %d | %s | %.2f | %.4f | %s | %.2f | %.4f | %.4f | %.0f | %s |\n", $1, $2, $3, $4, $5, flush, $7, $8, $9, $11, fs
  }' "$summary_tsv"
  echo

  echo '### Diagnostics'
  echo
  echo '| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |'
  echo '|---|---:|---:|---:|---:|'
  awk -F'\t' '$17=="memory" { printf "| %s | %.0f | %.2f | %.2f | %.2f |\n", $1, $13, $14, $15, $16 }' "$summary_tsv"
  echo

  echo '## File sink'
  echo
  echo '| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |'
  echo '|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|'
  awk -F'\t' '$17=="file" {
    flush = ($6 >= 0) ? sprintf("%.2f", $6) : "-"
    fs = ($12 >= 0) ? sprintf("%.0f", $12) : "-"
    printf "| %s | %d | %s | %.2f | %.4f | %s | %.2f | %.4f | %.4f | %.0f | %s |\n", $1, $2, $3, $4, $5, flush, $7, $8, $9, $11, fs
  }' "$summary_tsv"
  echo

  echo '### Diagnostics'
  echo
  echo '| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |'
  echo '|---|---:|---:|---:|---:|'
  awk -F'\t' '$17=="file" { printf "| %s | %.0f | %.2f | %.2f | %.2f |\n", $1, $13, $14, $15, $16 }' "$summary_tsv"
  echo
} > "$OUT_MD"

echo "Saved: $OUT_MD"
