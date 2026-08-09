#!/bin/bash
# Parse WSL log_bench.log E2E means (5 runs), mirroring run_all_benches.ps1 log-parse.
DIR=${1:-/mnt/c/Personal/Projects/LibNei/libnei-src/bench/results/wsl_20260809}
awk '
  /^[^ ]/ && /:$/ {
    cur=$0; sub(/:$/,"",cur); sub(/ \(File:.*\)/,"",cur)
    if(!(cur in n)){n[cur]=0; lps[cur]=0; lps2[cur]=0}
  }
  /E2E logs\/sec:/ {
    if(cur!=""){
      v=$3; gsub(/[^0-9.eE+-]/,"",v)
      lps[cur]+=v; lps2[cur]+=v*v; n[cur]++
    }
  }
  END {
    for(k in n){
      if(n[k]>0){
        m=lps[k]/n[k]
        printf "| %s | %d | %.0f /s |\n", k, n[k], m
      }
    }
  }
' "$DIR/log_bench.log" | sort
