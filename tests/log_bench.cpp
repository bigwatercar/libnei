#include <cstdio>
#include <cstdint>
#include <cstring>
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

void run_log_benchmark(const std::string &name, std::function<void()> log_func, int iterations = 1000000) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  BenchmarkConfigGuard guard;
  guard.set_primary_sink(&sink);

  auto start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iterations; ++i) {
    log_func();
  }
  nei_log_flush();
  auto end = std::chrono::high_resolution_clock::now();

  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  double avg_time = static_cast<double>(duration.count()) / iterations;

  std::cout << name << ":\n";
  std::cout << "  Iterations: " << iterations << "\n";
  std::cout << "  Total time: " << duration.count() << " microseconds\n";
  std::cout << "  Average time per log: " << avg_time << " microseconds\n";
  std::cout << "  Logs per second: " << (1000000.0 / avg_time) << "\n\n";
}

void run_file_log_benchmark(const std::string &name,
                            std::function<void()> log_func,
                            const std::string &filename,
                            int iterations = 100000) {
  // Create directory if it doesn't exist
#ifdef _WIN32
  _mkdir("C:\\var");
#else
  mkdir("C:/var", 0755);
#endif

  nei_log_sink_st *file_sink = nei_log_create_default_file_sink(filename.c_str());
  if (!file_sink) {
    std::cout << "Failed to create file sink for " << filename << "\n";
    return;
  }

  {
    BenchmarkConfigGuard guard;
    guard.set_primary_sink(file_sink);

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
      log_func();
    }
    nei_log_flush();
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    double avg_time = static_cast<double>(duration.count()) / iterations;

    std::cout << name << " (File: " << filename << "):\n";
    std::cout << "  Iterations: " << iterations << "\n";
    std::cout << "  Total time: " << duration.count() << " microseconds\n";
    std::cout << "  Average time per log: " << avg_time << " microseconds\n";
    std::cout << "  Logs per second: " << (1000000.0 / avg_time) << "\n";
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
  (void)argc;
  (void)argv;

  std::cout << "Log Performance Benchmark\n";
  std::cout << "=========================\n\n";

  // Benchmark different log levels
  run_log_benchmark("Log Info", []() {
    nei_llog(NEI_LOG_DEFAULT_CONFIG_ID, NEI_LOG_LEVEL_INFO, __FILE__, __LINE__, "benchmark", "test message %s", "test");
  });

  run_log_benchmark("Log Warn", []() {
    nei_llog(NEI_LOG_DEFAULT_CONFIG_ID, NEI_LOG_LEVEL_WARN, __FILE__, __LINE__, "benchmark", "test message %s", "test");
  });

  run_log_benchmark("Log Error", []() {
    nei_llog(
        NEI_LOG_DEFAULT_CONFIG_ID, NEI_LOG_LEVEL_ERROR, __FILE__, __LINE__, "benchmark", "test message %s", "test");
  });

  // Benchmark with formatting
  run_log_benchmark("Log with Formatting", []() {
    nei_llog(NEI_LOG_DEFAULT_CONFIG_ID,
             NEI_LOG_LEVEL_INFO,
             __FILE__,
             __LINE__,
             "benchmark",
             "number=%d, string=%s, float=%.2f",
             42,
             "hello",
             3.14);
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
  for (int i = 0; i < iterations; ++i) {
    nei_vlog(NEI_LOG_DEFAULT_CONFIG_ID, 1, __FILE__, __LINE__, "benchmark", "verbose message %s", "verbose");
  }
  nei_log_flush();
  auto end = std::chrono::high_resolution_clock::now();

  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  double avg_time = static_cast<double>(duration.count()) / iterations;

  std::cout << "Log Verbose:\n";
  std::cout << "  Iterations: " << iterations << "\n";
  std::cout << "  Total time: " << duration.count() << " microseconds\n";
  std::cout << "  Average time per log: " << avg_time << " microseconds\n";
  std::cout << "  Logs per second: " << (1000000.0 / avg_time) << "\n\n";

  std::cout << "File-based Log Performance Benchmark (SSD)\n";
  std::cout << "=========================================\n\n";

  // File-based benchmarks (higher iteration count than typical quick runs; I/O bound)
  run_file_log_benchmark(
      "File Log Info",
      []() {
        nei_llog(
            NEI_LOG_DEFAULT_CONFIG_ID, NEI_LOG_LEVEL_INFO, __FILE__, __LINE__, "benchmark", "test message %s", "test");
      },
      "C:\\var\\log_bench_info.log");

  run_file_log_benchmark(
      "File Log Warn",
      []() {
        nei_llog(
            NEI_LOG_DEFAULT_CONFIG_ID, NEI_LOG_LEVEL_WARN, __FILE__, __LINE__, "benchmark", "test message %s", "test");
      },
      "C:\\var\\log_bench_warn.log");

  run_file_log_benchmark(
      "File Log Error",
      []() {
        nei_llog(
            NEI_LOG_DEFAULT_CONFIG_ID, NEI_LOG_LEVEL_ERROR, __FILE__, __LINE__, "benchmark", "test message %s", "test");
      },
      "C:\\var\\log_bench_error.log");

  run_file_log_benchmark(
      "File Log with Formatting",
      []() {
        nei_llog(NEI_LOG_DEFAULT_CONFIG_ID,
                 NEI_LOG_LEVEL_INFO,
                 __FILE__,
                 __LINE__,
                 "benchmark",
                 "number=%d, string=%s, count=%d",
                 42,
                 "hello",
                 3);
      },
      "C:\\var\\log_bench_format.log");

  run_file_log_benchmark(
      "File Log Verbose",
      []() {
        nei_vlog(NEI_LOG_DEFAULT_CONFIG_ID, 1, __FILE__, __LINE__, "benchmark", "verbose message %s", "verbose");
      },
      "C:\\var\\log_bench_verbose.log");

  return 0;
}