#!/usr/bin/env bash

set -euo pipefail

# Log compare benchmark (Linux) - rebuild + run + summarize
# Usage:
#   ./bench/rebuild_and_bench_log_compare.sh
#   ./bench/rebuild_and_bench_log_compare.sh --runs 10 --skip-build

RUNS=5
SKIP_BUILD=0
BUILD_DIR=""
OUTPUT_DIR=""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_MD="$REPO_ROOT/log_bench_compare_latest.md"

usage() {
  cat <<'EOF'
Usage: rebuild_and_bench_log_compare.sh [options]

Options:
  --runs N         Number of runs (default: 5)
  --skip-build     Skip cmake --build
  --build-dir PATH Build directory override
  --output-dir PATH Log output directory (default: <repo>/bench/output)
  --out-md PATH    Markdown output path (default: <repo>/log_bench_compare_latest.md)
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
    if [[ -x "$d/bench/log_bench_compare" ]]; then
      BUILD_DIR="$d"
      break
    fi
  done
fi

if [[ -z "$BUILD_DIR" ]]; then
  BUILD_DIR="$REPO_ROOT/build-linux"
fi

EXE_PATH="$BUILD_DIR/bench/log_bench_compare"

if [[ -z "$OUTPUT_DIR" ]]; then
  OUTPUT_DIR="$REPO_ROOT/bench/output"
fi

mkdir -p "$OUTPUT_DIR"

LOG_FILES=(
  "$OUTPUT_DIR/nei_cmp_simple.log"
  "$OUTPUT_DIR/spdlog_cmp_simple.log"
  "$OUTPUT_DIR/nei_cmp_multi.log"
  "$OUTPUT_DIR/spdlog_cmp_multi.log"
  "$OUTPUT_DIR/nei_cmp_literal.log"
  "$OUTPUT_DIR/nei_cmp_vlog_literal.log"
  "$OUTPUT_DIR/nei_cmp_simple_sync.log"
  "$OUTPUT_DIR/spdlog_cmp_simple_sync.log"
  "$OUTPUT_DIR/nei_cmp_multi_sync.log"
  "$OUTPUT_DIR/spdlog_cmp_multi_sync.log"
  "$OUTPUT_DIR/nei_cmp_literal_sync.log"
  "$OUTPUT_DIR/nei_cmp_vlog_literal_sync.log"
  "$OUTPUT_DIR/nei_cmp_simple_strict.log"
  "$OUTPUT_DIR/spdlog_cmp_simple_strict.log"
  "$OUTPUT_DIR/nei_cmp_multi_strict.log"
  "$OUTPUT_DIR/spdlog_cmp_multi_strict.log"
)

if [[ ! -d "$BUILD_DIR" ]]; then
  echo "Build directory not found: $BUILD_DIR" >&2
  exit 1
fi

pkill -f log_bench_compare >/dev/null 2>&1 || true
pkill -f nei_tests >/dev/null 2>&1 || true

if [[ "$SKIP_BUILD" -eq 0 ]]; then
  echo "==> Building..."
  cmake --build "$BUILD_DIR" --target nei neixx log_bench_compare
  echo
fi

if [[ ! -x "$EXE_PATH" ]]; then
  echo "Executable not found: $EXE_PATH" >&2
  exit 1
fi

raw_tsv="$(mktemp)"
summary_tsv="$(mktemp)"
trap 'rm -f "$raw_tsv" "$summary_tsv"' EXIT

canonical_name() {
  local name="$1"
  name="${name#[[]*] }"
  name="${name#file }"
  name="$(echo "$name" | sed -E 's/[[:space:]]*\((flush request each log|async logger \+ flush request each log|sync flush each log)\)$//')"
  case "$name" in
    simple*) echo "simple" ;;
    "multi printf (fmt_plan cache miss)") echo "multi (cache miss)" ;;
    multi*) echo "multi" ;;
    llog_literal*|"literal only") echo "literal" ;;
    vlog_literal*) echo "vlog_literal" ;;
    *) echo "$name" ;;
  esac
}

parse_run() {
  local run_idx="$1"
  local run_out="$2"
  awk -v run="$run_idx" '
  function reset_cur() {
    iterations=""; total_us=""; avg_us=""; logs_sec="";
    prod=""; flush_wait=""; wakeups=""; hwm=""; file_size="";
  }
  function emit_cur() {
    if (bench != "" && logs_sec != "") {
      print run "\t" section "\t" bench "\t" iterations "\t" total_us "\t" avg_us "\t" logs_sec "\t" prod "\t" flush_wait "\t" wakeups "\t" hwm "\t" file_size;
      bench="";
      reset_cur();
    }
  }
  BEGIN { section=""; bench=""; reset_cur(); }
  {
    line=$0

    if (match(line, /^---[[:space:]]+(.+)[[:space:]]+---$/, m)) {
      section=m[1]
      next
    }

    if (match(line, /^\[[^]]+\].+$/, m)) {
      emit_cur();
      bench=line;
      reset_cur();
      next
    }

    if (bench == "") next;

    if (match(line, /^[[:space:]]+Iterations:[[:space:]]+([0-9]+)/, m)) { iterations=m[1]; next; }
    if (match(line, /^[[:space:]]+Total time:[[:space:]]+([0-9.]+) microseconds$/, m)) { total_us=m[1]; next; }
    if (match(line, /^[[:space:]]+Average time per log:[[:space:]]+([0-9.]+) microseconds$/, m)) { avg_us=m[1]; next; }
    if (match(line, /^[[:space:]]+Logs per second:[[:space:]]+([0-9.eE+-]+)/, m)) { logs_sec=m[1]; next; }
    if (match(line, /^[[:space:]]+Runtime stats:[[:space:]]+producer_spins=([0-9]+),[[:space:]]+flush_wait_loops=([0-9]+),[[:space:]]+consumer_wakeups=([0-9]+),[[:space:]]+ring_hwm=([0-9]+)/, m)) {
      prod=m[1]; flush_wait=m[2]; wakeups=m[3]; hwm=m[4]; next;
    }
    if (match(line, /^[[:space:]]+File size:[[:space:]]+(-?[0-9]+) bytes$/, m)) {
      file_size=m[1];
      emit_cur();
      next;
    }

    if (line ~ /^[[:space:]]*$/ && section == "Memory (async, minimal sink)") {
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

  echo "  Running log_bench_compare (run $r/$RUNS)"
  run_out="$(mktemp)"
  "$EXE_PATH" "$OUTPUT_DIR" > "$run_out"
  parse_run "$r" "$run_out"
  rm -f "$run_out"
done

section_order_csv='Memory (async, minimal sink),File (async file sink),File (per-call flush request over async pipeline),File (strict sync flush semantics)'
benchmark_order_csv='[NEI]  simple %s,[spdlog] simple {},[NEI]  multi printf,[spdlog] multi fmt,[NEI]  multi printf (fmt_plan cache miss),[NEI]  llog_literal (opaque body),[spdlog] literal only,[NEI]  vlog_literal (opaque body),[NEI] file simple %s,[spdlog] file simple {},[NEI] file multi,[spdlog] file multi,[NEI] file llog_literal,[NEI] file vlog_literal,[NEI] file sync simple (flush request each log),[spdlog] file sync simple (async logger + flush request each log),[NEI] file sync multi (flush request each log),[spdlog] file sync multi (async logger + flush request each log),[NEI] file sync llog_literal (flush request each log),[NEI] file sync vlog_literal (flush request each log),[NEI] file strict simple (sync flush each log),[spdlog] file strict simple (sync flush each log),[NEI] file strict multi (sync flush each log),[spdlog] file strict multi (sync flush each log)'

awk -F'\t' -v OFS='\t' -v section_order_csv="$section_order_csv" -v benchmark_order_csv="$benchmark_order_csv" '
BEGIN {
  split(section_order_csv, sarr, ",")
  for (i = 1; i <= length(sarr); i++) srank[sarr[i]] = i
  split(benchmark_order_csv, barr, ",")
  for (i = 1; i <= length(barr); i++) brank[barr[i]] = i
}
{
  section=$2
  bench=$3
  key=section SUBSEP bench
  c[key]++
  iterations[key]=$4
  s_total[key]+=($5+0)
  s_avg[key]+=($6+0)
  s_lps[key]+=($7+0)
  if ($8 != "") { s_prod[key]+=($8+0); c_prod[key]++ }
  if ($9 != "") { s_flush[key]+=($9+0); c_flush[key]++ }
  if ($10 != "") { s_wake[key]+=($10+0); c_wake[key]++ }
  if ($11 != "") { s_hwm[key]+=($11+0); c_hwm[key]++ }
  if ($12 != "") { s_file[key]+=($12+0); c_file[key]++ }

  delta=($6+0)-mean[key]
  mean[key]+=delta/c[key]
  m2[key]+=delta*(($6+0)-mean[key])
}
END {
  for (k in c) {
    split(k, p, SUBSEP)
    section=p[1]; bench=p[2]
    std=(c[k] > 1) ? sqrt(m2[k]/c[k]) : 0
    file=(c_file[k] > 0) ? s_file[k]/c_file[k] : -1
    prod=(c_prod[k] > 0) ? s_prod[k]/c_prod[k] : -1
    flush=(c_flush[k] > 0) ? s_flush[k]/c_flush[k] : -1
    wake=(c_wake[k] > 0) ? s_wake[k]/c_wake[k] : -1
    hwm=(c_hwm[k] > 0) ? s_hwm[k]/c_hwm[k] : -1

    lib="unknown"
    if (bench ~ /^\[NEI\]/) lib="NEI"
    else if (bench ~ /^\[spdlog\]/) lib="spdlog"

    print section, bench, lib, c[k], iterations[k],
          s_total[k]/c[k], s_avg[k]/c[k], std, s_lps[k]/c[k], file,
          prod, flush, wake, hwm,
          srank[section]+0, brank[bench]+0
  }
}
' "$raw_tsv" | sort -t $'\t' -k15,15n -k16,16n > "$summary_tsv"

echo
printf 'Log Compare Benchmark  -  %s  (Runs: %d)\n\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$RUNS"

for section in \
  'Memory (async, minimal sink)' \
  'File (async file sink)' \
  'File (per-call flush request over async pipeline)' \
  'File (strict sync flush semantics)'; do

  echo "$section"
  echo
  printf '%-52s  %4s  %12s  %10s  %12s  %12s\n' \
    'Benchmark' 'Runs' 'Avg us/log' 'Stddev' 'Logs/sec' 'File bytes'
  printf '%-52s  %4s  %12s  %10s  %12s  %12s\n' \
    '----------------------------------------------------' '----' '------------' '----------' '------------' '------------'

  awk -F'\t' -v sec="$section" '
  $1==sec {
    fs = ($10 >= 0) ? sprintf("%.0f", $10) : "-"
    printf "%-52s  %4d  %12.4f  %10.4f  %12.0f  %12s\n", $2, $4, $7, $8, $9, fs
  }
  ' "$summary_tsv"
  echo
done

echo 'Diagnostics (NEI rows only)'
echo
printf '%-52s  %12s  %12s  %12s  %10s\n' 'Benchmark' 'Prod spins' 'Flush waits' 'Wakeups' 'Ring HWM'
printf '%-52s  %12s  %12s  %12s  %10s\n' \
  '----------------------------------------------------' '------------' '------------' '------------' '----------'
awk -F'\t' '$3=="NEI" {
  p=($11>=0)?sprintf("%.0f",$11):"-"
  f=($12>=0)?sprintf("%.2f",$12):"-"
  w=($13>=0)?sprintf("%.2f",$13):"-"
  h=($14>=0)?sprintf("%.2f",$14):"-"
  printf "%-52s  %12s  %12s  %12s  %10s\n", $2, p, f, w, h
}' "$summary_tsv"

best_line="$(awk -F'\t' 'NR==1 || $7 < min { min=$7; line=$0 } END{ print line }' "$summary_tsv")"
worst_line="$(awk -F'\t' 'NR==1 || $7 > max { max=$7; line=$0 } END{ print line }' "$summary_tsv")"

{
  echo '# Log Compare Benchmark'
  echo
  echo "Generated: $(date '+%Y-%m-%d %H:%M:%S')"
  echo "Runs: $RUNS"
  echo 'Build: Release'
  echo

  echo '## Highlights'
  echo
  echo '| Type | Benchmark | Section | Avg us/log | Avg logs/sec |'
  echo '|---|---|---|---:|---:|'
  awk -F'\t' -v row="$best_line" 'BEGIN{ split(row,f,"\t"); printf "| Best | %s | %s | %.4f | %.0f |\n", f[2], f[1], f[7], f[9] }'
  awk -F'\t' -v row="$worst_line" 'BEGIN{ split(row,f,"\t"); printf "| Worst | %s | %s | %.4f | %.0f |\n", f[2], f[1], f[7], f[9] }'
  echo

  echo '## Memory vs File Ratios'
  echo
  echo '| Library | Scenario | Memory avg us/log | File avg us/log | File/Memory x |'
  echo '|---|---|---:|---:|---:|'
  awk -F'\t' '
  function canon(name) {
    n=name
    sub(/^\[[^]]+\][[:space:]]*/,"",n)
    sub(/^file[[:space:]]+/,"",n)
    gsub(/[[:space:]]*\((flush request each log|async logger \+ flush request each log|sync flush each log)\)$/,"",n)
    if (n ~ /^simple[[:space:]]/) return "simple"
    if (n == "multi printf (fmt_plan cache miss)") return "multi (cache miss)"
    if (n ~ /^multi[[:space:]]/) return "multi"
    if (n ~ /^llog_literal([[:space:]]|$)/ || n == "literal only") return "literal"
    if (n ~ /^vlog_literal([[:space:]]|$)/) return "vlog_literal"
    return n
  }
  {
    k=$3 SUBSEP canon($2)
    if ($1=="Memory (async, minimal sink)") mem[k]=$7
    else if ($1=="File (async file sink)") file[k]=$7
  }
  END {
    for (k in mem) {
      if ((k in file) && mem[k] > 0) {
        split(k,p,SUBSEP)
        printf "| %s | %s | %.4f | %.4f | %.2f |\n", p[1], p[2], mem[k], file[k], file[k]/mem[k]
      }
    }
  }
  ' "$summary_tsv" | sort
  echo

  for section in \
    'Memory (async, minimal sink)' \
    'File (async file sink)' \
    'File (per-call flush request over async pipeline)' \
    'File (strict sync flush semantics)'; do
    echo "## $section"
    echo
    echo '| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |'
    echo '|---|---:|---:|---:|---:|---:|---:|---:|'
    awk -F'\t' -v sec="$section" '
    $1==sec {
      fs = ($10 >= 0) ? sprintf("%.0f", $10) : "-"
      printf "| %s | %d | %s | %.2f | %.4f | %.4f | %.0f | %s |\n", $2, $4, $5, $6, $7, $8, $9, fs
    }
    ' "$summary_tsv"
    echo

    echo '### Diagnostics'
    echo
    echo '| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |'
    echo '|---|---:|---:|---:|---:|'
    awk -F'\t' -v sec="$section" '
    $1==sec {
      p=($11>=0)?sprintf("%.0f",$11):"-"
      f=($12>=0)?sprintf("%.2f",$12):"-"
      w=($13>=0)?sprintf("%.2f",$13):"-"
      h=($14>=0)?sprintf("%.2f",$14):"-"
      printf "| %s | %s | %s | %s | %s |\n", $2, p, f, w, h
    }
    ' "$summary_tsv"
    echo
  done
} > "$OUT_MD"

echo "Saved: $OUT_MD"
