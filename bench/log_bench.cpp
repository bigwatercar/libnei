#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <iostream>
#include <functional>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "nei/log/log.h"

namespace {
struct LogCollector {
  std::mutex mu;
  std::atomic<uint64_t> record_count{0};
  int last_verbose = -1;
};

extern "C" void
CollectLevelLog(const nei_log_sink_st *sink, nei_log_level_e level, const char *message, size_t length) {
  (void)level;
  (void)message;
  (void)length;
  auto *collector = static_cast<LogCollector *>(sink->opaque);
  collector->record_count.fetch_add(1U, std::memory_order_relaxed);
}

extern "C" void CollectVerboseLog(const nei_log_sink_st *sink, int verbose, const char *message, size_t length) {
  (void)message;
  (void)length;
  auto *collector = static_cast<LogCollector *>(sink->opaque);
  std::lock_guard<std::mutex> lock(collector->mu);
  collector->last_verbose = verbose;
  collector->record_count.fetch_add(1U, std::memory_order_relaxed);
}

struct BenchmarkConfigGuard {
  nei_log_sink_st *saved[NEI_LOG_MAX_SINKS_OF_CONFIG]{};

  BenchmarkConfigGuard() {
    nei_log_config_st *cfg = nei_log_default_config();
    for (size_t i = 0; i < NEI_LOG_MAX_SINKS_OF_CONFIG; ++i) {
      saved[i] = cfg->sinks[i];
      cfg->sinks[i] = nullptr;
    }
  }

  void set_primary_sink(nei_log_sink_st *sink) {
    nei_log_config_st *cfg = nei_log_default_config();
    cfg->sinks[0] = sink;
    for (size_t i = 1; i < NEI_LOG_MAX_SINKS_OF_CONFIG; ++i) {
      cfg->sinks[i] = nullptr;
    }
  }

  ~BenchmarkConfigGuard() {
    nei_log_config_st *cfg = nei_log_default_config();
    for (size_t i = 0; i < NEI_LOG_MAX_SINKS_OF_CONFIG; ++i) {
      cfg->sinks[i] = saved[i];
    }
  }

  BenchmarkConfigGuard(const BenchmarkConfigGuard &) = delete;
  BenchmarkConfigGuard &operator=(const BenchmarkConfigGuard &) = delete;
};
} // namespace

static bool ensure_output_dir(const std::string &out_dir) {
  if (out_dir.empty()) {
    return false;
  }
#ifdef _WIN32
  if (_mkdir(out_dir.c_str()) == 0 || errno == EEXIST) {
    return true;
  }
#else
  if (mkdir(out_dir.c_str(), 0755) == 0 || errno == EEXIST) {
    return true;
  }
#endif
  return false;
}

static std::string join_path(const std::string &dir, const char *name) {
  if (dir.empty()) {
    return std::string(name);
  }
  const char last = dir[dir.size() - 1U];
  if (last == '/' || last == '\\') {
    return dir + name;
  }
#ifdef _WIN32
  return dir + "\\" + name;
#else
  return dir + "/" + name;
#endif
}

void run_log_benchmark(const std::string &name, std::function<void()> log_func, int iterations = 1000000) {
  LogCollector collector;
  nei_log_perf_stats_st stats = {};
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  BenchmarkConfigGuard guard;
  guard.set_primary_sink(&sink);

  nei_log_reset_perf_stats_for_test();

  auto start_enqueue = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iterations; ++i) {
    log_func();
  }
  auto end_enqueue = std::chrono::high_resolution_clock::now();

  auto start_flush = std::chrono::high_resolution_clock::now();
  nei_log_flush();
  auto end_flush = std::chrono::high_resolution_clock::now();

  (void)nei_log_get_perf_stats_for_test(&stats);

  auto enqueue_duration = std::chrono::duration_cast<std::chrono::microseconds>(end_enqueue - start_enqueue);
  auto flush_duration = std::chrono::duration_cast<std::chrono::microseconds>(end_flush - start_flush);
  auto e2e_duration = std::chrono::duration_cast<std::chrono::microseconds>(end_flush - start_enqueue);
  double enqueue_avg = static_cast<double>(enqueue_duration.count()) / iterations;
  double e2e_avg = static_cast<double>(e2e_duration.count()) / iterations;

  std::cout << name << ":\n";
  std::cout << "  Iterations: " << iterations << "\n";
  std::cout << "  Enqueue total: " << enqueue_duration.count() << " microseconds\n";
  std::cout << "  Enqueue avg per log: " << enqueue_avg << " microseconds\n";
  std::cout << "  Flush total: " << flush_duration.count() << " microseconds\n";
  std::cout << "  E2E total: " << e2e_duration.count() << " microseconds\n";
  std::cout << "  E2E avg per log: " << e2e_avg << " microseconds\n";
  std::cout << "  Enqueue logs/sec: " << (1000000.0 / enqueue_avg) << "\n";
  std::cout << "  E2E logs/sec: " << (1000000.0 / e2e_avg) << "\n";
  std::cout << "  Runtime stats: producer_spins=" << stats.producer_spin_loops
            << ", flush_wait_loops=" << stats.flush_wait_loops
            << ", consumer_wakeups=" << stats.consumer_wakeups
            << ", ring_hwm=" << stats.ring_high_watermark << "\n\n";
}

void run_vlog_benchmark(const std::string &name, std::function<void()> log_func, int iterations = 1000000) {
  LogCollector collector;
  nei_log_perf_stats_st stats = {};
  nei_log_sink_st sink = {};
  sink.vlog = CollectVerboseLog;
  sink.opaque = &collector;

  BenchmarkConfigGuard guard;
  guard.set_primary_sink(&sink);

  nei_log_reset_perf_stats_for_test();

  auto start_enqueue = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iterations; ++i) {
    log_func();
  }
  auto end_enqueue = std::chrono::high_resolution_clock::now();

  auto start_flush = std::chrono::high_resolution_clock::now();
  nei_log_flush();
  auto end_flush = std::chrono::high_resolution_clock::now();

  (void)nei_log_get_perf_stats_for_test(&stats);

  auto enqueue_duration = std::chrono::duration_cast<std::chrono::microseconds>(end_enqueue - start_enqueue);
  auto flush_duration = std::chrono::duration_cast<std::chrono::microseconds>(end_flush - start_flush);
  auto e2e_duration = std::chrono::duration_cast<std::chrono::microseconds>(end_flush - start_enqueue);
  double enqueue_avg = static_cast<double>(enqueue_duration.count()) / iterations;
  double e2e_avg = static_cast<double>(e2e_duration.count()) / iterations;

  std::cout << name << ":\n";
  std::cout << "  Iterations: " << iterations << "\n";
  std::cout << "  Enqueue total: " << enqueue_duration.count() << " microseconds\n";
  std::cout << "  Enqueue avg per log: " << enqueue_avg << " microseconds\n";
  std::cout << "  Flush total: " << flush_duration.count() << " microseconds\n";
  std::cout << "  E2E total: " << e2e_duration.count() << " microseconds\n";
  std::cout << "  E2E avg per log: " << e2e_avg << " microseconds\n";
  std::cout << "  Enqueue logs/sec: " << (1000000.0 / enqueue_avg) << "\n";
  std::cout << "  E2E logs/sec: " << (1000000.0 / e2e_avg) << "\n";
  std::cout << "  Runtime stats: producer_spins=" << stats.producer_spin_loops
            << ", flush_wait_loops=" << stats.flush_wait_loops
            << ", consumer_wakeups=" << stats.consumer_wakeups
            << ", ring_hwm=" << stats.ring_high_watermark << "\n\n";
}

void run_file_log_benchmark(const std::string &name,
                            std::function<void()> log_func,
                            const std::string &filename,
                            int iterations = 100000) {
  nei_log_perf_stats_st stats = {};

  nei_log_sink_st *file_sink = nei_log_create_default_file_sink(filename.c_str(), NULL);
  if (!file_sink) {
    std::cout << "Failed to create file sink for " << filename << "\n";
    return;
  }

  {
    BenchmarkConfigGuard guard;
    guard.set_primary_sink(file_sink);

    nei_log_reset_perf_stats_for_test();

    auto start_enqueue = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
      log_func();
    }
    auto end_enqueue = std::chrono::high_resolution_clock::now();

    auto start_flush = std::chrono::high_resolution_clock::now();
    nei_log_flush();
    auto end_flush = std::chrono::high_resolution_clock::now();

    (void)nei_log_get_perf_stats_for_test(&stats);

    auto enqueue_duration = std::chrono::duration_cast<std::chrono::microseconds>(end_enqueue - start_enqueue);
    auto flush_duration = std::chrono::duration_cast<std::chrono::microseconds>(end_flush - start_flush);
    auto e2e_duration = std::chrono::duration_cast<std::chrono::microseconds>(end_flush - start_enqueue);
    double enqueue_avg = static_cast<double>(enqueue_duration.count()) / iterations;
    double e2e_avg = static_cast<double>(e2e_duration.count()) / iterations;

    std::cout << name << " (File: " << filename << "):\n";
    std::cout << "  Iterations: " << iterations << "\n";
    std::cout << "  Enqueue total: " << enqueue_duration.count() << " microseconds\n";
    std::cout << "  Enqueue avg per log: " << enqueue_avg << " microseconds\n";
    std::cout << "  Flush total: " << flush_duration.count() << " microseconds\n";
    std::cout << "  E2E total: " << e2e_duration.count() << " microseconds\n";
    std::cout << "  E2E avg per log: " << e2e_avg << " microseconds\n";
    std::cout << "  Enqueue logs/sec: " << (1000000.0 / enqueue_avg) << "\n";
    std::cout << "  E2E logs/sec: " << (1000000.0 / e2e_avg) << "\n";
    std::cout << "  Runtime stats: producer_spins=" << stats.producer_spin_loops
              << ", flush_wait_loops=" << stats.flush_wait_loops
              << ", consumer_wakeups=" << stats.consumer_wakeups
              << ", ring_hwm=" << stats.ring_high_watermark << "\n";
  }

  // Destroy sink first
  nei_log_destroy_sink(file_sink);

  // Then check file size
  std::ifstream check_file(filename, std::ios::binary | std::ios::ate);
  long file_size = check_file ? static_cast<long>(check_file.tellg()) : -1;
  check_file.close();

  std::cout << "  File size: " << file_size << " bytes\n\n";
}

int main(int argc, char **argv) {
  if (argc < 2 || argv[1] == NULL || argv[1][0] == '\0') {
    std::cerr << "Usage: " << ((argv != NULL && argv[0] != NULL) ? argv[0] : "log_bench") << " <output_dir>\n";
    return 1;
  }

  const std::string out_dir = argv[1];
  if (!ensure_output_dir(out_dir)) {
    std::cerr << "Failed to create output dir: " << out_dir << "\n";
    return 1;
  }

  std::cout << "Log Performance Benchmark\n";
  std::cout << "=========================\n\n";
  std::cout << "Output dir: " << out_dir << "\n\n";

  // Benchmark different log levels
  run_log_benchmark("Log Info", []() {
    nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_INFO, __FILE__, __LINE__, "benchmark", "test message %s", "test");
  });

  run_log_benchmark("Log Warn", []() {
    nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_WARN, __FILE__, __LINE__, "benchmark", "test message %s", "test");
  });

  run_log_benchmark("Log Error", []() {
    nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_ERROR, __FILE__, __LINE__, "benchmark", "test message %s", "test");
  });

  // Benchmark with formatting
  run_log_benchmark("Log with Formatting", []() {
    nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE,
             NEI_L_INFO,
             __FILE__,
             __LINE__,
             "benchmark",
             "number=%d, string=%s, float=%.2f",
             42,
             "hello",
             3.14);
  });

  run_log_benchmark("Log Info (literal)", []() {
    static const char body[] = "literal message no printf";
    nei_llog_literal(
        NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_INFO, __FILE__, __LINE__, "benchmark", body, sizeof(body) - 1U);
  });

  // Benchmark verbose logging
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.vlog = CollectVerboseLog;
  sink.opaque = &collector;

  BenchmarkConfigGuard guard;
  guard.set_primary_sink(&sink);

  auto start = std::chrono::high_resolution_clock::now();
  const int iterations = 1000000;
  nei_log_perf_stats_st verbose_stats = {};
  nei_log_reset_perf_stats_for_test();
  for (int i = 0; i < iterations; ++i) {
    nei_vlog(NEI_LOG_DEFAULT_CONFIG_HANDLE, 1, __FILE__, __LINE__, "benchmark", "verbose message %s", "verbose");
  }
  auto enqueue_end = std::chrono::high_resolution_clock::now();

  auto flush_start = std::chrono::high_resolution_clock::now();
  nei_log_flush();
  auto end = std::chrono::high_resolution_clock::now();

  (void)nei_log_get_perf_stats_for_test(&verbose_stats);

  auto enqueue_duration = std::chrono::duration_cast<std::chrono::microseconds>(enqueue_end - start);
  auto flush_duration = std::chrono::duration_cast<std::chrono::microseconds>(end - flush_start);
  auto e2e_duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  double enqueue_avg = static_cast<double>(enqueue_duration.count()) / iterations;
  double e2e_avg = static_cast<double>(e2e_duration.count()) / iterations;

  std::cout << "Log Verbose:\n";
  std::cout << "  Iterations: " << iterations << "\n";
  std::cout << "  Enqueue total: " << enqueue_duration.count() << " microseconds\n";
  std::cout << "  Enqueue avg per log: " << enqueue_avg << " microseconds\n";
  std::cout << "  Flush total: " << flush_duration.count() << " microseconds\n";
  std::cout << "  E2E total: " << e2e_duration.count() << " microseconds\n";
  std::cout << "  E2E avg per log: " << e2e_avg << " microseconds\n";
  std::cout << "  Enqueue logs/sec: " << (1000000.0 / enqueue_avg) << "\n";
  std::cout << "  E2E logs/sec: " << (1000000.0 / e2e_avg) << "\n";
  std::cout << "  Runtime stats: producer_spins=" << verbose_stats.producer_spin_loops
            << ", flush_wait_loops=" << verbose_stats.flush_wait_loops
            << ", consumer_wakeups=" << verbose_stats.consumer_wakeups
            << ", ring_hwm=" << verbose_stats.ring_high_watermark << "\n\n";

  run_vlog_benchmark("Log Verbose (literal)", []() {
    static const char body[] = "verbose literal body";
    nei_vlog_literal(NEI_LOG_DEFAULT_CONFIG_HANDLE, 1, __FILE__, __LINE__, "benchmark", body, sizeof(body) - 1U);
  });

  std::cout << "File-based Log Performance Benchmark (SSD)\n";
  std::cout << "=========================================\n\n";

  // File-based benchmarks (higher iteration count than typical quick runs; I/O bound)
  run_file_log_benchmark(
      "File Log Info",
      []() {
        nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_INFO, __FILE__, __LINE__, "benchmark", "test message %s", "test");
      },
      join_path(out_dir, "log_bench_info.log"));

  run_file_log_benchmark(
      "File Log Warn",
      []() {
        nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_WARN, __FILE__, __LINE__, "benchmark", "test message %s", "test");
      },
      join_path(out_dir, "log_bench_warn.log"));

  run_file_log_benchmark(
      "File Log Error",
      []() {
        nei_llog(
            NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_ERROR, __FILE__, __LINE__, "benchmark", "test message %s", "test");
      },
      join_path(out_dir, "log_bench_error.log"));

  run_file_log_benchmark(
      "File Log with Formatting",
      []() {
        nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE,
                 NEI_L_INFO,
                 __FILE__,
                 __LINE__,
                 "benchmark",
                 "number=%d, string=%s, count=%d",
                 42,
                 "hello",
                 3);
      },
              join_path(out_dir, "log_bench_format.log"));

  run_file_log_benchmark(
      "File Log Verbose",
      []() {
        nei_vlog(NEI_LOG_DEFAULT_CONFIG_HANDLE, 1, __FILE__, __LINE__, "benchmark", "verbose message %s", "verbose");
      },
      join_path(out_dir, "log_bench_verbose.log"));

  run_file_log_benchmark(
      "File Log Info (literal)",
      []() {
        static const char body[] = "literal line no printf";
        nei_llog_literal(
            NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_INFO, __FILE__, __LINE__, "benchmark", body, sizeof(body) - 1U);
      },
      join_path(out_dir, "log_bench_info_literal.log"));

  run_file_log_benchmark(
      "File Log Verbose (literal)",
      []() {
        static const char body[] = "verbose literal line";
        nei_vlog_literal(NEI_LOG_DEFAULT_CONFIG_HANDLE, 1, __FILE__, __LINE__, "benchmark", body, sizeof(body) - 1U);
      },
      join_path(out_dir, "log_bench_verbose_literal.log"));

  return 0;
}