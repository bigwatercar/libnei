# task_thread_bench Baseline and Gate

Date: 2026-05-16
Platform: Windows
Build: Release (windows-vs2022-release-shared)
Binary: build/windows-vs2022-release-shared/bench/Release/task_thread_bench.exe

## 1. Benchmark Method

### 1.1 Runtime configuration
- Task count per round: 100000
- Benchmark thread model: nei::Thread + TaskRunner
- Payload: `AddTaskBodyNoArgs` (one minimal add operation)
- TaskTracing: forced ON during benchmark run
- Completion signal: sentinel task + WaitableEvent

### 1.2 Correctness checks (must pass)
The benchmark itself validates execution correctness:
- `failed == 0`
- `sentinel_failed == 0`
- `executed_tasks == posted_ok`
- `sum_sink == expected_sum` where `expected_sum = posted_ok * 3`
- `verify_ok == 1`

Benchmark process exits with non-zero if any check fails.

### 1.3 Command used
```powershell
& "build/windows-vs2022-release-shared/bench/Release/task_thread_bench.exe" 100000
```

## 2. Current Baseline Results (30 rounds, TaskTracing ON)

Per-round summary was collected and aggregated.

### 2.1 Throughput statistics (total_throughput)
- Mean: 6938273 tasks/sec
- Min: 5869957 tasks/sec
- Max: 7534376 tasks/sec
- StdDev: 361715
- CV: 5.21%
- P50: 6961074 tasks/sec
- P90: 7359760 tasks/sec
- P95: 7360843 tasks/sec
- P99: 7534376 tasks/sec

### 2.2 Correctness stability
- Total failed posts across 30 rounds: 0
- Rounds with correctness deviation: 0
- All rounds satisfy:
  - `verify_ok = 1`
  - `failed = 0`
  - `sentinel_failed = 0`
  - `executed_tasks = posted_ok`

## 3. Quick Recheck (5 rounds, TaskTracing ON)

- Mean total_throughput: 7097953 tasks/sec
- Mean avg_total_ns_per_task: 142.13 ns
- Correctness: all rounds passed (`verify_ok=1`, no failures)

## 4. Suggested Gate Policy

### 4.1 Mandatory correctness gate (every run)
For every benchmark round:
- `verify_ok == 1`
- `failed == 0`
- `sentinel_failed == 0`
- `executed_tasks == posted_ok`

Fail immediately if any condition is violated.

### 4.2 Performance gate (recommended)
- PR gate (fast, 5 rounds):
  - Mean `total_throughput >= 6600000` tasks/sec
  - No single round below `5600000` tasks/sec
- Nightly gate (stable, 30 rounds):
  - Mean `total_throughput >= 6800000` tasks/sec
  - P50 `total_throughput >= 6900000` tasks/sec
  - CV `<= 8.0%`

These thresholds are intentionally below current baseline to reduce false positives from environment jitter while still detecting real regressions.

## 5. Notes
- This document is intended as the baseline reference for future optimization and regression checks.
- If hardware/power policy changes, refresh baseline and adjust thresholds with a new dated section.
