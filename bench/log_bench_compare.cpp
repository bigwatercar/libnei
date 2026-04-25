/**
 * NEI vs spdlog - aligned micro-benchmark scaffold.
 *
 * Methodology (read before interpreting numbers):
 * - Memory: both libraries enqueue asynchronously; sink does only an atomic increment (no I/O, no
 *   full string retention). Timings include producer loop + flush that drains the async pipeline.
 * - File (async): both sides use async file logging; benchmark deletes target files before each run so
 *   results are not skewed by append growth.
 * - File (per-call delivery): both sides use async logger + flush after each log call, so each iteration
 *   includes per-call delivery pressure on the async pipeline.
 * - Single-threaded producer; not a multi-writer contention test.
 * - spdlog uses fmt-style "{}" formatting; NEI uses printf-style "%" formatting - work split
 *   between caller formatting vs binary serialization differs by design.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

#include <spdlog/async.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include "nei/log/log.h"

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace {

constexpr int kMemoryIters = 1'000'000;
constexpr int kFileIters = 100'000;
/**
 * Sync file mode: flush-after-each-log is intentionally expensive.
 * Keep this high to stress per-call delivery behavior consistently.
 */
constexpr int kFileSyncIters = 10'000;

/**
 * Strict sync mode uses fewer iterations because both sides execute true
 * per-call disk flush behavior.
 */
constexpr int kFileStrictSyncIters = 5'000;

class ScopedEnvVar {
public:
  ScopedEnvVar(const char *name, const char *value)
      : name_(name), had_old_(false) {
    if (name_ == nullptr || name_[0] == '\0') {
      return;
    }
    const char *old = std::getenv(name_);
    if (old != nullptr) {
      had_old_ = true;
      old_value_ = old;
    }
    set(value);
  }

  ~ScopedEnvVar() {
    if (name_ == nullptr || name_[0] == '\0') {
      return;
    }
    if (had_old_) {
      set(old_value_.c_str());
    } else {
      unset();
    }
  }

private:
  void set(const char *value) {
    if (value == nullptr) {
      unset();
      return;
    }
#if defined(_WIN32)
    (void)_putenv_s(name_, value);
#else
    (void)setenv(name_, value, 1);
#endif
  }

  void unset() {
#if defined(_WIN32)
    (void)_putenv_s(name_, "");
#else
    (void)unsetenv(name_);
#endif
  }

  const char *name_;
  bool had_old_;
  std::string old_value_;
};

bool ensure_out_dir(const std::string &out_dir) {
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

std::string out_file(const std::string &out_dir, const char *name) {
  if (out_dir.empty()) {
    return std::string(name);
  }
  const char last = out_dir[out_dir.size() - 1U];
  if (last == '/' || last == '\\') {
    return out_dir + name;
  }
#ifdef _WIN32
  return out_dir + "\\" + name;
#else
  return out_dir + "/" + name;
#endif
}

void print_stats(const std::string &label, int iterations, int64_t micros) {
  const double avg = static_cast<double>(micros) / static_cast<double>(iterations);
  std::cout << label << "\n";
  std::cout << "  Iterations: " << iterations << "\n";
  std::cout << "  Total time: " << micros << " microseconds\n";
  std::cout << "  Average time per log: " << avg << " microseconds\n";
  std::cout << "  Logs per second: " << (1000000.0 / avg) << "\n\n";
}

void print_stats(const std::string &label, int iterations, int64_t micros, const nei_log_perf_stats_st &stats) {
  const double avg = static_cast<double>(micros) / static_cast<double>(iterations);
  std::cout << label << "\n";
  std::cout << "  Iterations: " << iterations << "\n";
  std::cout << "  Total time: " << micros << " microseconds\n";
  std::cout << "  Average time per log: " << avg << " microseconds\n";
  std::cout << "  Logs per second: " << (1000000.0 / avg) << "\n";
  std::cout << "  Runtime stats: producer_spins=" << stats.producer_spin_loops
            << ", flush_wait_loops=" << stats.flush_wait_loops
            << ", consumer_wakeups=" << stats.consumer_wakeups
            << ", ring_hwm=" << stats.ring_high_watermark << "\n\n";
}

void print_file_size(const std::string &path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  const long sz = f ? static_cast<long>(f.tellg()) : -1L;
  std::cout << "  File size: " << sz << " bytes\n\n";
}

// ---------------------------------------------------------------------------
// NEI: counter-only sink + config guard (same idea as log_bench.cpp)
// ---------------------------------------------------------------------------

struct NeiCollector {
  std::atomic<uint64_t> record_count{0};
};

extern "C" void nei_collect_only(const nei_log_sink_st *sink, nei_log_level_e level, const char *msg, size_t len) {
  (void)level;
  (void)msg;
  (void)len;
  static_cast<NeiCollector *>(sink->opaque)->record_count.fetch_add(1U, std::memory_order_relaxed);
}

extern "C" void nei_collect_vlog_only(const nei_log_sink_st *sink, int verbose, const char *msg, size_t len) {
  (void)verbose;
  (void)msg;
  (void)len;
  static_cast<NeiCollector *>(sink->opaque)->record_count.fetch_add(1U, std::memory_order_relaxed);
}

struct NeiConfigGuard {
  nei_log_sink_st *saved[NEI_LOG_MAX_SINKS_OF_CONFIG]{};
  int saved_log_thread_id = 0;
  int saved_log_to_console = 0;

  NeiConfigGuard() {
    nei_log_config_st *cfg = nei_log_default_config();
    saved_log_thread_id = cfg->log_thread_id;
    saved_log_to_console = cfg->log_to_console;
    // Fair-compare mode: do not inject tid= payload by default.
    cfg->log_thread_id = 0;
    cfg->log_to_console = 0;
    for (size_t i = 0; i < NEI_LOG_MAX_SINKS_OF_CONFIG; ++i) {
      saved[i] = cfg->sinks[i];
      cfg->sinks[i] = nullptr;
    }
  }

  void set_primary(nei_log_sink_st *s) {
    nei_log_config_st *cfg = nei_log_default_config();
    cfg->sinks[0] = s;
    for (size_t i = 1; i < NEI_LOG_MAX_SINKS_OF_CONFIG; ++i) {
      cfg->sinks[i] = nullptr;
    }
  }

  ~NeiConfigGuard() {
    nei_log_config_st *cfg = nei_log_default_config();
    cfg->log_thread_id = saved_log_thread_id;
    cfg->log_to_console = saved_log_to_console;
    for (size_t i = 0; i < NEI_LOG_MAX_SINKS_OF_CONFIG; ++i) {
      cfg->sinks[i] = saved[i];
    }
  }

  NeiConfigGuard(const NeiConfigGuard &) = delete;
  NeiConfigGuard &operator=(const NeiConfigGuard &) = delete;
};

struct NeiBenchResult {
  int64_t micros = -1;
  nei_log_perf_stats_st stats = {};
};

template <class F>
NeiBenchResult time_nei_memory_ms(F &&f, int iters) {
  NeiCollector col;
  nei_log_sink_st sink{};
  sink.llog = nei_collect_only;
  sink.opaque = &col;
  NeiConfigGuard guard;
  guard.set_primary(&sink);
  nei_log_reset_perf_stats_for_test();
  const auto t0 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iters; ++i) {
    f();
  }
  nei_log_flush();
  const auto t1 = std::chrono::high_resolution_clock::now();
  NeiBenchResult result;
  result.micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  (void)nei_log_get_perf_stats_for_test(&result.stats);
  return result;
}

template <class F>
NeiBenchResult time_nei_memory_vlog_ms(F &&f, int iters) {
  NeiCollector col;
  nei_log_sink_st sink{};
  sink.vlog = nei_collect_vlog_only;
  sink.opaque = &col;
  NeiConfigGuard guard;
  guard.set_primary(&sink);
  nei_log_reset_perf_stats_for_test();
  const auto t0 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iters; ++i) {
    f();
  }
  nei_log_flush();
  const auto t1 = std::chrono::high_resolution_clock::now();
  NeiBenchResult result;
  result.micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  (void)nei_log_get_perf_stats_for_test(&result.stats);
  return result;
}

template <class F>
NeiBenchResult time_nei_file_ms(F &&f, int iters, const char *path) {
  (void)std::remove(path);
  nei_log_sink_st *fs = nei_log_create_default_file_sink(path, NULL);
  if (!fs) {
    return {};
  }
  NeiBenchResult result;
  result.micros = -1;
  {
    NeiConfigGuard guard;
    guard.set_primary(fs);
    nei_log_reset_perf_stats_for_test();
    const auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iters; ++i) {
      f();
    }
    nei_log_flush();
    const auto t1 = std::chrono::high_resolution_clock::now();
    result.micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    (void)nei_log_get_perf_stats_for_test(&result.stats);
  }
  nei_log_destroy_sink(fs);
  return result;
}

/** NEI file sink + nei_log_flush() after each log (caller blocks until that record is processed). */
template <class F>
NeiBenchResult time_nei_file_sync_ms(F &&f, int iters, const char *path) {
  (void)std::remove(path);
  nei_log_sink_st *fs = nei_log_create_default_file_sink(path, NULL);
  if (!fs) {
    return {};
  }
  NeiBenchResult result;
  result.micros = -1;
  {
    NeiConfigGuard guard;
    guard.set_primary(fs);
    nei_log_reset_perf_stats_for_test();
    const auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iters; ++i) {
      f();
      nei_log_flush();
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    result.micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    (void)nei_log_get_perf_stats_for_test(&result.stats);
  }
  nei_log_destroy_sink(fs);
  return result;
}

/**
 * NEI strict sync: force sink to fflush each record and disable sink-side write batching,
 * then keep per-call nei_log_flush as delivery barrier.
 */
template <class F>
NeiBenchResult time_nei_file_strict_sync_ms(F &&f, int iters, const char *path) {
  (void)std::remove(path);
  nei_log_default_file_sink_options_st opts = nei_log_default_file_sink_options();
  opts.flush_interval    = 1U; /* fflush after every record */
  opts.write_batch_bytes = 0U; /* disable batch writing */
  nei_log_sink_st *fs = nei_log_create_default_file_sink(path, &opts);
  if (!fs) {
    return {};
  }
  NeiBenchResult result;
  result.micros = -1;
  {
    NeiConfigGuard guard;
    guard.set_primary(fs);
    nei_log_reset_perf_stats_for_test();
    const auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iters; ++i) {
      f();
      nei_log_flush();
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    result.micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    (void)nei_log_get_perf_stats_for_test(&result.stats);
  }
  nei_log_destroy_sink(fs);
  return result;
}

// ---------------------------------------------------------------------------
// spdlog: async + counter sink (memory) or basic_file_sink_mt (file)
// ---------------------------------------------------------------------------

template <typename Mutex>
class counter_sink : public spdlog::sinks::base_sink<Mutex> {
public:
  explicit counter_sink(std::atomic<uint64_t> *c)
      : counter_(c) {
  }

protected:
  void sink_it_(const spdlog::details::log_msg &) override {
    counter_->fetch_add(1U, std::memory_order_relaxed);
  }

  void flush_() override {
  }

private:
  std::atomic<uint64_t> *counter_;
};

template <class F>
int64_t time_spdlog_memory_ms(F &&f, int iters) {
  std::atomic<uint64_t> counter{0};
  spdlog::init_thread_pool(32768, 1);
  auto sink = std::make_shared<counter_sink<std::mutex>>(&counter);
  auto logger = std::make_shared<spdlog::async_logger>(
      "nei_cmp", spdlog::sinks_init_list{sink}, spdlog::thread_pool(), spdlog::async_overflow_policy::block);
  const auto t0 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iters; ++i) {
    f(logger);
  }
  logger->flush();
  const auto t1 = std::chrono::high_resolution_clock::now();
  const int64_t micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  spdlog::shutdown();
  return micros;
}

template <class F>
int64_t time_spdlog_file_ms(F &&f, int iters, const std::string &path) {
  (void)std::remove(path.c_str());
  spdlog::init_thread_pool(32768, 1);
  auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path, true);
  auto logger = std::make_shared<spdlog::async_logger>(
      "nei_cmp_file", spdlog::sinks_init_list{file_sink}, spdlog::thread_pool(), spdlog::async_overflow_policy::block);
  const auto t0 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iters; ++i) {
    f(logger);
  }
  logger->flush();
  const auto t1 = std::chrono::high_resolution_clock::now();
  const int64_t micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  spdlog::shutdown();
  return micros;
}

/** Async file + flush each log call (parity with NEI per-call flush pressure). */
template <class F>
int64_t time_spdlog_file_sync_ms(F &&f, int iters, const std::string &path) {
  (void)std::remove(path.c_str());
  spdlog::init_thread_pool(32768, 1);
  auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path, true);
  auto logger = std::make_shared<spdlog::async_logger>(
      "nei_cmp_sync", spdlog::sinks_init_list{sink}, spdlog::thread_pool(), spdlog::async_overflow_policy::block);
  const auto t0 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iters; ++i) {
    f(logger);
    logger->flush();
  }
  logger->flush();
  const auto t1 = std::chrono::high_resolution_clock::now();
  const int64_t micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  spdlog::shutdown();
  return micros;
}

/**
 * Strict sync path for spdlog: synchronous logger + flush per call.
 */
template <class F>
int64_t time_spdlog_file_strict_sync_ms(F &&f, int iters, const std::string &path) {
  (void)std::remove(path.c_str());
  auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path, true);
  auto logger = std::make_shared<spdlog::logger>("nei_cmp_sync_strict", spdlog::sinks_init_list{sink});
  const auto t0 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iters; ++i) {
    f(logger);
    logger->flush();
  }
  logger->flush();
  const auto t1 = std::chrono::high_resolution_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2 || argv[1] == NULL || argv[1][0] == '\0') {
    std::cerr << "Usage: " << ((argv != NULL && argv[0] != NULL) ? argv[0] : "log_bench_compare")
              << " <output_dir>\n";
    return 1;
  }

  const std::string out_dir = argv[1];
  if (!ensure_out_dir(out_dir)) {
    std::cerr << "Failed to create output dir: " << out_dir << "\n";
    return 1;
  }

  std::cout << "NEI vs spdlog - aligned benchmark scaffold\n";
  std::cout << "==========================================\n";
  std::cout << "Output dir: " << out_dir << "\n";
  std::cout << "Memory:     " << kMemoryIters << " iters; sink = atomic++ only; time includes flush.\n";
  std::cout << "File async: " << kFileIters << " iters; async + file sink; both sides delete old files before run.\n";
  std::cout << "Per-call:   " << kFileSyncIters << " iters; async logger + flush request after each log on both sides.\n";
  std::cout << "Strict:     " << kFileStrictSyncIters << " iters; per-call flush with strict sync semantics.\n";
  std::cout << "Fairness knobs: NEI log_thread_id forced OFF during benchmark.\n\n";

  std::cout << "--- Memory (async, minimal sink) ---\n\n";

  {
    const auto result = time_nei_memory_ms(
          [] {
            nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_INFO, __FILE__, __LINE__, "bench", "test message %s", "test");
          },
          kMemoryIters);
    print_stats("[NEI]  simple %s", kMemoryIters, result.micros, result.stats);
  }

  print_stats(
      "[spdlog] simple {}",
      kMemoryIters,
      time_spdlog_memory_ms([](const std::shared_ptr<spdlog::logger> &log) { log->info("test message {}", "test"); },
                            kMemoryIters));

  {
    const auto result = time_nei_memory_ms(
                  [] {
                    nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE,
                             NEI_L_INFO,
                             __FILE__,
                             __LINE__,
                             "bench",
                             "number=%d, string=%s, float=%.2f",
                             42,
                             "hello",
                             3.14);
                  },
                  kMemoryIters);
    print_stats("[NEI]  multi printf", kMemoryIters, result.micros, result.stats);
  }

  print_stats("[spdlog] multi fmt",
              kMemoryIters,
              time_spdlog_memory_ms(
                  [](const std::shared_ptr<spdlog::logger> &log) {
                    log->info("number={}, string={}, float={:.2f}", 42, "hello", 3.14);
                  },
                  kMemoryIters));

  {
    /**
     * Cache-miss scenario: alternate between two different fmt string literals on every
     * call so the TLS fmt_plan cache invalidates each iteration. This measures the
     * worst-case producer overhead when the cache is always cold (e.g. many distinct
     * call sites interleaved on the same thread, or a logging helper that wraps
     * multiple format strings).
     *
     * The two fmt strings use the same argument types (int, const char*, double) as the
     * cache-hit "multi printf" benchmark above, so the only variable between the two
     * scenarios is cache hit vs miss — not format string complexity.
     */
    const auto result = time_nei_memory_ms(
        [] {
          /* Two distinct string-literal array addresses with identical argument
           * structure to the cache-hit "multi printf" scenario (%d, %s, %.2f). */
          static const char fmt_a[] = "number=%d, string=%s, float=%.2f";
          static const char fmt_b[] = "count=%d, label=%s, value=%.2f";
          static int s_toggle = 0;
          const int idx = s_toggle & 1;
          s_toggle ^= 1;
          nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE,
                   NEI_L_INFO,
                   __FILE__,
                   __LINE__,
                   "bench",
                   idx ? fmt_b : fmt_a,
                   idx ? 99 : 42,
                   idx ? "world" : "hello",
                   idx ? 2.71 : 3.14);
        },
        kMemoryIters);
    print_stats("[NEI]  multi printf (fmt_plan cache miss)", kMemoryIters, result.micros, result.stats);
  }

  {
    const auto result = time_nei_memory_ms(
          [] {
            static const char body[] = "info-only";
            nei_llog_literal(
                NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_INFO, __FILE__, __LINE__, "bench", body, sizeof(body) - 1U);
          },
          kMemoryIters);
    print_stats("[NEI]  llog_literal (opaque body)", kMemoryIters, result.micros, result.stats);
  }

  print_stats(
      "[spdlog] literal only",
      kMemoryIters,
      time_spdlog_memory_ms([](const std::shared_ptr<spdlog::logger> &log) { log->info("info-only"); }, kMemoryIters));

  {
    const auto result = time_nei_memory_vlog_ms(
                  [] {
                    static const char body[] = "verbose-literal";
                    nei_vlog_literal(
                        NEI_LOG_DEFAULT_CONFIG_HANDLE, 1, __FILE__, __LINE__, "bench", body, sizeof(body) - 1U);
                  },
                  kMemoryIters);
    print_stats("[NEI]  vlog_literal (opaque body)", kMemoryIters, result.micros, result.stats);
  }

  std::cout << "--- File (async file sink) ---\n\n";

  const std::string nei_simple = out_file(out_dir, "nei_cmp_simple.log");
  const std::string spd_simple = out_file(out_dir, "spdlog_cmp_simple.log");
  const std::string nei_multi = out_file(out_dir, "nei_cmp_multi.log");
  const std::string spd_multi = out_file(out_dir, "spdlog_cmp_multi.log");

  {
    const auto result = time_nei_file_ms(
        [] {
          nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_INFO, __FILE__, __LINE__, "bench", "test message %s", "test");
        },
        kFileIters,
        nei_simple.c_str());
    if (result.micros < 0) {
      std::cout << "[NEI] file simple: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file simple %s", kFileIters, result.micros, result.stats);
      print_file_size(nei_simple);
    }
  }

  {
    const int64_t us =
        time_spdlog_file_ms([](const std::shared_ptr<spdlog::logger> &log) { log->info("test message {}", "test"); },
                            kFileIters,
                            spd_simple);
    print_stats("[spdlog] file simple {}", kFileIters, us);
    print_file_size(spd_simple);
  }

  {
    const auto result = time_nei_file_ms(
        [] {
          nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE,
                   NEI_L_INFO,
                   __FILE__,
                   __LINE__,
                   "bench",
                   "number=%d, string=%s, count=%d",
                   42,
                   "hello",
                   3);
        },
        kFileIters,
        nei_multi.c_str());
    if (result.micros < 0) {
      std::cout << "[NEI] file multi: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file multi", kFileIters, result.micros, result.stats);
      print_file_size(nei_multi);
    }
  }

  {
    const int64_t us = time_spdlog_file_ms(
        [](const std::shared_ptr<spdlog::logger> &log) { log->info("number={}, string={}, count={}", 42, "hello", 3); },
        kFileIters,
        spd_multi);
    print_stats("[spdlog] file multi", kFileIters, us);
    print_file_size(spd_multi);
  }

  const std::string nei_lit = out_file(out_dir, "nei_cmp_literal.log");
  const std::string nei_vlit = out_file(out_dir, "nei_cmp_vlog_literal.log");

  {
    const auto result = time_nei_file_ms(
        [] {
          static const char body[] = "file literal body";
          nei_llog_literal(
              NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_INFO, __FILE__, __LINE__, "bench", body, sizeof(body) - 1U);
        },
        kFileIters,
        nei_lit.c_str());
    if (result.micros < 0) {
      std::cout << "[NEI] file llog_literal: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file llog_literal", kFileIters, result.micros, result.stats);
      print_file_size(nei_lit);
    }
  }

  {
    const auto result = time_nei_file_ms(
        [] {
          static const char body[] = "file vlog literal";
          nei_vlog_literal(NEI_LOG_DEFAULT_CONFIG_HANDLE, 1, __FILE__, __LINE__, "bench", body, sizeof(body) - 1U);
        },
        kFileIters,
        nei_vlit.c_str());
    if (result.micros < 0) {
      std::cout << "[NEI] file vlog_literal: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file vlog_literal", kFileIters, result.micros, result.stats);
      print_file_size(nei_vlit);
    }
  }

  std::cout << "--- File (per-call flush request over async pipeline) ---\n\n";

  const std::string nei_simple_sync = out_file(out_dir, "nei_cmp_simple_sync.log");
  const std::string spd_simple_sync = out_file(out_dir, "spdlog_cmp_simple_sync.log");
  const std::string nei_multi_sync = out_file(out_dir, "nei_cmp_multi_sync.log");
  const std::string spd_multi_sync = out_file(out_dir, "spdlog_cmp_multi_sync.log");

  {
    const auto result = time_nei_file_sync_ms(
        [] {
          nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_INFO, __FILE__, __LINE__, "bench", "test message %s", "test");
        },
        kFileSyncIters,
        nei_simple_sync.c_str());
    if (result.micros < 0) {
      std::cout << "[NEI] file sync simple: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file sync simple (flush request each log)", kFileSyncIters, result.micros, result.stats);
      print_file_size(nei_simple_sync);
    }
  }

  {
    const int64_t us = time_spdlog_file_sync_ms(
        [](const std::shared_ptr<spdlog::logger> &log) { log->info("test message {}", "test"); },
        kFileSyncIters,
        spd_simple_sync);
    print_stats("[spdlog] file sync simple (async logger + flush request each log)", kFileSyncIters, us);
    print_file_size(spd_simple_sync);
  }

  {
    const auto result = time_nei_file_sync_ms(
        [] {
          nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE,
                   NEI_L_INFO,
                   __FILE__,
                   __LINE__,
                   "bench",
                   "number=%d, string=%s, count=%d",
                   42,
                   "hello",
                   3);
        },
        kFileSyncIters,
        nei_multi_sync.c_str());
    if (result.micros < 0) {
      std::cout << "[NEI] file sync multi: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file sync multi (flush request each log)", kFileSyncIters, result.micros, result.stats);
      print_file_size(nei_multi_sync);
    }
  }

  {
    const int64_t us = time_spdlog_file_sync_ms(
        [](const std::shared_ptr<spdlog::logger> &log) { log->info("number={}, string={}, count={}", 42, "hello", 3); },
        kFileSyncIters,
        spd_multi_sync);
    print_stats("[spdlog] file sync multi (async logger + flush request each log)", kFileSyncIters, us);
    print_file_size(spd_multi_sync);
  }

  const std::string nei_lit_sync = out_file(out_dir, "nei_cmp_literal_sync.log");
  const std::string nei_vlit_sync = out_file(out_dir, "nei_cmp_vlog_literal_sync.log");

  {
    const auto result = time_nei_file_sync_ms(
        [] {
          static const char body[] = "sync literal body";
          nei_llog_literal(
              NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_INFO, __FILE__, __LINE__, "bench", body, sizeof(body) - 1U);
        },
        kFileSyncIters,
        nei_lit_sync.c_str());
    if (result.micros < 0) {
      std::cout << "[NEI] file sync llog_literal: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file sync llog_literal (flush request each log)", kFileSyncIters, result.micros, result.stats);
      print_file_size(nei_lit_sync);
    }
  }

  {
    const auto result = time_nei_file_sync_ms(
        [] {
          static const char body[] = "sync vlog literal";
          nei_vlog_literal(NEI_LOG_DEFAULT_CONFIG_HANDLE, 1, __FILE__, __LINE__, "bench", body, sizeof(body) - 1U);
        },
        kFileSyncIters,
        nei_vlit_sync.c_str());
    if (result.micros < 0) {
      std::cout << "[NEI] file sync vlog_literal: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file sync vlog_literal (flush request each log)", kFileSyncIters, result.micros, result.stats);
      print_file_size(nei_vlit_sync);
    }
  }

  std::cout << "--- File (strict sync flush semantics) ---\n\n";

  const std::string nei_simple_strict = out_file(out_dir, "nei_cmp_simple_strict.log");
  const std::string spd_simple_strict = out_file(out_dir, "spdlog_cmp_simple_strict.log");
  const std::string nei_multi_strict = out_file(out_dir, "nei_cmp_multi_strict.log");
  const std::string spd_multi_strict = out_file(out_dir, "spdlog_cmp_multi_strict.log");

  {
    const auto result = time_nei_file_strict_sync_ms(
        [] {
          nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_INFO, __FILE__, __LINE__, "bench", "test message %s", "test");
        },
        kFileStrictSyncIters,
        nei_simple_strict.c_str());
    if (result.micros < 0) {
      std::cout << "[NEI] file strict simple: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file strict simple (sync flush each log)", kFileStrictSyncIters, result.micros, result.stats);
      print_file_size(nei_simple_strict);
    }
  }

  {
    const int64_t us = time_spdlog_file_strict_sync_ms(
        [](const std::shared_ptr<spdlog::logger> &log) { log->info("test message {}", "test"); },
        kFileStrictSyncIters,
        spd_simple_strict);
    print_stats("[spdlog] file strict simple (sync flush each log)", kFileStrictSyncIters, us);
    print_file_size(spd_simple_strict);
  }

  {
    const auto result = time_nei_file_strict_sync_ms(
        [] {
          nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE,
                   NEI_L_INFO,
                   __FILE__,
                   __LINE__,
                   "bench",
                   "number=%d, string=%s, count=%d",
                   42,
                   "hello",
                   3);
        },
        kFileStrictSyncIters,
        nei_multi_strict.c_str());
    if (result.micros < 0) {
      std::cout << "[NEI] file strict multi: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file strict multi (sync flush each log)", kFileStrictSyncIters, result.micros, result.stats);
      print_file_size(nei_multi_strict);
    }
  }

  {
    const int64_t us = time_spdlog_file_strict_sync_ms(
        [](const std::shared_ptr<spdlog::logger> &log) { log->info("number={}, string={}, count={}", 42, "hello", 3); },
        kFileStrictSyncIters,
        spd_multi_strict);
    print_stats("[spdlog] file strict multi (sync flush each log)", kFileStrictSyncIters, us);
    print_file_size(spd_multi_strict);
  }

  return 0;
}
