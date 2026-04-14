/**
 * NEI vs spdlog — aligned micro-benchmark scaffold.
 *
 * Methodology (read before interpreting numbers):
 * - Memory: both libraries enqueue asynchronously; sink does only an atomic increment (no I/O, no
 *   full string retention). Timings include producer loop + flush that drains the async pipeline.
 * - File (async): both sides use async file logging; benchmark deletes target files before each run so
 *   results are not skewed by append growth.
 * - File (per-call delivery): both sides use async logger + flush after each log call, so each iteration
 *   includes per-call delivery pressure on the async pipeline.
 * - Single-threaded producer; not a multi-writer contention test.
 * - spdlog uses fmt-style "{}" formatting; NEI uses printf-style "%" formatting — work split
 *   between caller formatting vs binary serialization differs by design.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
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
/** Sync file mode: NEI flush-per-log is costly; keep count lower than kFileIters. */
constexpr int kFileSyncIters = 10'000;

void ensure_out_dir() {
#ifdef _WIN32
  _mkdir("C:\\var");
#else
  mkdir("/tmp/nei_bench", 0755);
#endif
}

std::string out_file(const char *name) {
#ifdef _WIN32
  return std::string("C:\\var\\") + name;
#else
  return std::string("/tmp/nei_bench/") + name;
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

template <class F>
int64_t time_nei_memory_ms(F &&f, int iters) {
  NeiCollector col;
  nei_log_sink_st sink{};
  sink.llog = nei_collect_only;
  sink.opaque = &col;
  NeiConfigGuard guard;
  guard.set_primary(&sink);
  const auto t0 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iters; ++i) {
    f();
  }
  nei_log_flush();
  const auto t1 = std::chrono::high_resolution_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
}

template <class F>
int64_t time_nei_memory_vlog_ms(F &&f, int iters) {
  NeiCollector col;
  nei_log_sink_st sink{};
  sink.vlog = nei_collect_vlog_only;
  sink.opaque = &col;
  NeiConfigGuard guard;
  guard.set_primary(&sink);
  const auto t0 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iters; ++i) {
    f();
  }
  nei_log_flush();
  const auto t1 = std::chrono::high_resolution_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
}

template <class F>
int64_t time_nei_file_ms(F &&f, int iters, const char *path) {
  (void)std::remove(path);
  nei_log_sink_st *fs = nei_log_create_default_file_sink(path);
  if (!fs) {
    return -1;
  }
  int64_t micros = -1;
  {
    NeiConfigGuard guard;
    guard.set_primary(fs);
    const auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iters; ++i) {
      f();
    }
    nei_log_flush();
    const auto t1 = std::chrono::high_resolution_clock::now();
    micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  }
  nei_log_destroy_sink(fs);
  return micros;
}

/** NEI file sink + nei_log_flush() after each log (caller blocks until that record is processed). */
template <class F>
int64_t time_nei_file_sync_ms(F &&f, int iters, const char *path) {
  (void)std::remove(path);
  nei_log_sink_st *fs = nei_log_create_default_file_sink(path);
  if (!fs) {
    return -1;
  }
  int64_t micros = -1;
  {
    NeiConfigGuard guard;
    guard.set_primary(fs);
    const auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iters; ++i) {
      f();
      nei_log_flush();
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  }
  nei_log_destroy_sink(fs);
  return micros;
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

} // namespace

int main() {
  std::cout << "NEI vs spdlog — aligned benchmark scaffold\n";
  std::cout << "==========================================\n";
  std::cout << "Memory:     " << kMemoryIters << " iters; sink = atomic++ only; time includes flush.\n";
  std::cout << "File async: " << kFileIters
            << " iters; async + file sink; both sides delete old files before run.\n";
  std::cout << "Per-call:   " << kFileSyncIters
            << " iters; async logger + flush after each log on both sides.\n";
  std::cout << "Fairness knobs: NEI log_thread_id forced OFF during benchmark.\n\n";

  std::cout << "--- Memory (async, minimal sink) ---\n\n";

  print_stats("[NEI]  simple %s",
              kMemoryIters,
              time_nei_memory_ms(
                  [] {
                    nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE,
                             NEI_L_INFO,
                             __FILE__,
                             __LINE__,
                             "bench",
                             "test message %s",
                             "test");
                  },
                  kMemoryIters));

  print_stats(
      "[spdlog] simple {}",
      kMemoryIters,
      time_spdlog_memory_ms([](const std::shared_ptr<spdlog::logger> &log) { log->info("test message {}", "test"); },
                            kMemoryIters));

  print_stats("[NEI]  multi printf",
              kMemoryIters,
              time_nei_memory_ms(
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
                  kMemoryIters));

  print_stats("[spdlog] multi fmt",
              kMemoryIters,
              time_spdlog_memory_ms(
                  [](const std::shared_ptr<spdlog::logger> &log) {
                    log->info("number={}, string={}, float={:.2f}", 42, "hello", 3.14);
                  },
                  kMemoryIters));

  print_stats("[NEI]  llog_literal (opaque body)",
              kMemoryIters,
              time_nei_memory_ms(
                  [] {
                    static const char body[] = "info-only";
                    nei_llog_literal(NEI_LOG_DEFAULT_CONFIG_HANDLE,
                                     NEI_L_INFO,
                                     __FILE__,
                                     __LINE__,
                                     "bench",
                                     body,
                                     sizeof(body) - 1U);
                  },
                  kMemoryIters));

  print_stats(
      "[spdlog] literal only",
      kMemoryIters,
      time_spdlog_memory_ms([](const std::shared_ptr<spdlog::logger> &log) { log->info("info-only"); }, kMemoryIters));

  print_stats("[NEI]  vlog_literal (opaque body)",
              kMemoryIters,
              time_nei_memory_vlog_ms(
                  [] {
                    static const char body[] = "verbose-literal";
                    nei_vlog_literal(
                        NEI_LOG_DEFAULT_CONFIG_HANDLE, 1, __FILE__, __LINE__, "bench", body, sizeof(body) - 1U);
                  },
                  kMemoryIters));

  std::cout << "--- File (async file sink) ---\n\n";
  ensure_out_dir();

  const std::string nei_simple = out_file("nei_cmp_simple.log");
  const std::string spd_simple = out_file("spdlog_cmp_simple.log");
  const std::string nei_multi = out_file("nei_cmp_multi.log");
  const std::string spd_multi = out_file("spdlog_cmp_multi.log");

  {
    const int64_t us = time_nei_file_ms(
        [] {
          nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE,
                   NEI_L_INFO,
                   __FILE__,
                   __LINE__,
                   "bench",
                   "test message %s",
                   "test");
        },
        kFileIters,
        nei_simple.c_str());
    if (us < 0) {
      std::cout << "[NEI] file simple: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file simple %s", kFileIters, us);
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
    const int64_t us = time_nei_file_ms(
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
    if (us < 0) {
      std::cout << "[NEI] file multi: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file multi", kFileIters, us);
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

  const std::string nei_lit = out_file("nei_cmp_literal.log");
  const std::string nei_vlit = out_file("nei_cmp_vlog_literal.log");

  {
    const int64_t us = time_nei_file_ms(
        [] {
          static const char body[] = "file literal body";
          nei_llog_literal(
              NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_INFO, __FILE__, __LINE__, "bench", body, sizeof(body) - 1U);
        },
        kFileIters,
        nei_lit.c_str());
    if (us < 0) {
      std::cout << "[NEI] file llog_literal: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file llog_literal", kFileIters, us);
      print_file_size(nei_lit);
    }
  }

  {
    const int64_t us = time_nei_file_ms(
        [] {
          static const char body[] = "file vlog literal";
          nei_vlog_literal(NEI_LOG_DEFAULT_CONFIG_HANDLE, 1, __FILE__, __LINE__, "bench", body, sizeof(body) - 1U);
        },
        kFileIters,
        nei_vlit.c_str());
    if (us < 0) {
      std::cout << "[NEI] file vlog_literal: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file vlog_literal", kFileIters, us);
      print_file_size(nei_vlit);
    }
  }

  std::cout << "--- File (per-call delivery over async pipeline) ---\n\n";

  const std::string nei_simple_sync = out_file("nei_cmp_simple_sync.log");
  const std::string spd_simple_sync = out_file("spdlog_cmp_simple_sync.log");
  const std::string nei_multi_sync = out_file("nei_cmp_multi_sync.log");
  const std::string spd_multi_sync = out_file("spdlog_cmp_multi_sync.log");

  {
    const int64_t us = time_nei_file_sync_ms(
        [] {
          nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE,
                   NEI_L_INFO,
                   __FILE__,
                   __LINE__,
                   "bench",
                   "test message %s",
                   "test");
        },
        kFileSyncIters,
        nei_simple_sync.c_str());
    if (us < 0) {
      std::cout << "[NEI] file sync simple: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file sync simple (flush each log)", kFileSyncIters, us);
      print_file_size(nei_simple_sync);
    }
  }

  {
    const int64_t us = time_spdlog_file_sync_ms(
        [](const std::shared_ptr<spdlog::logger> &log) { log->info("test message {}", "test"); },
        kFileSyncIters,
        spd_simple_sync);
    print_stats("[spdlog] file sync simple (async logger + flush each log)", kFileSyncIters, us);
    print_file_size(spd_simple_sync);
  }

  {
    const int64_t us = time_nei_file_sync_ms(
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
    if (us < 0) {
      std::cout << "[NEI] file sync multi: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file sync multi (flush each log)", kFileSyncIters, us);
      print_file_size(nei_multi_sync);
    }
  }

  {
    const int64_t us = time_spdlog_file_sync_ms(
        [](const std::shared_ptr<spdlog::logger> &log) { log->info("number={}, string={}, count={}", 42, "hello", 3); },
        kFileSyncIters,
        spd_multi_sync);
    print_stats("[spdlog] file sync multi (async logger + flush each log)", kFileSyncIters, us);
    print_file_size(spd_multi_sync);
  }

  const std::string nei_lit_sync = out_file("nei_cmp_literal_sync.log");
  const std::string nei_vlit_sync = out_file("nei_cmp_vlog_literal_sync.log");

  {
    const int64_t us = time_nei_file_sync_ms(
        [] {
          static const char body[] = "sync literal body";
          nei_llog_literal(
              NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_INFO, __FILE__, __LINE__, "bench", body, sizeof(body) - 1U);
        },
        kFileSyncIters,
        nei_lit_sync.c_str());
    if (us < 0) {
      std::cout << "[NEI] file sync llog_literal: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file sync llog_literal (flush each log)", kFileSyncIters, us);
      print_file_size(nei_lit_sync);
    }
  }

  {
    const int64_t us = time_nei_file_sync_ms(
        [] {
          static const char body[] = "sync vlog literal";
          nei_vlog_literal(NEI_LOG_DEFAULT_CONFIG_HANDLE, 1, __FILE__, __LINE__, "bench", body, sizeof(body) - 1U);
        },
        kFileSyncIters,
        nei_vlit_sync.c_str());
    if (us < 0) {
      std::cout << "[NEI] file sync vlog_literal: failed to create sink\n\n";
    } else {
      print_stats("[NEI] file sync vlog_literal (flush each log)", kFileSyncIters, us);
      print_file_size(nei_vlit_sync);
    }
  }

  return 0;
}
