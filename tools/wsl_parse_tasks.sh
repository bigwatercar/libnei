#!/bin/bash
# Parse WSL task-series bench logs into markdown rows (mean/stddev of Post throughput).
DIR=${1:-/mnt/c/Personal/Projects/LibNei/libnei-src/bench/results/wsl_20260809}

parse() { # file
  local f=$1 cur=""
  awk '
    /^--- / { cur=$0; gsub(/^--- | ---$/,"",cur); if(!(cur in n)){n[cur]=0} }
    /Post throughput: / {
      if(cur!=""){
        v=$3; gsub(/[^0-9.]/,"",v)
        s[cur]+=v; ss[cur]+=v*v; n[cur]++
      }
    }
    END {
      for(k in n){
        if(n[k]>0){
          m=s[k]/n[k]
          sd=sqrt(ss[k]/n[k]-m*m)
          printf "| %s | %d | %.0f /s | %.0f |\n", k, n[k], m, sd
        }
      }
    }
  ' "$f"
}

echo "== WSL task_thread (tracing ON, 10x1M) =="
parse "$DIR/task_thread_tracing_on.log"
echo ""
echo "== WSL task_thread (tracing OFF, 10x1M) =="
parse "$DIR/task_thread_tracing_off.log"
echo ""
echo "== WSL task_threadpool (5x1M) =="
parse "$DIR/task_threadpool.log"
echo ""
echo "== WSL task_threadpool_parallel (5x1M) =="
parse "$DIR/task_threadpool_parallel.log"
