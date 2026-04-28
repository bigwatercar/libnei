#include <gtest/gtest.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nei/log/log.h"

#if defined(_WIN32)
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace {
struct LogCollector {
  std::mutex mu;
  std::vector<std::string> messages;
  int last_verbose = -1;
};

extern "C" void
CollectLevelLog(const nei_log_sink_st *sink, nei_log_level_e level, const char *message, size_t length) {
  (void)level;
  auto *collector = static_cast<LogCollector *>(sink->opaque);
  std::lock_guard<std::mutex> lock(collector->mu);
  collector->messages.emplace_back(message, length);
}

extern "C" void CollectVerboseLog(const nei_log_sink_st *sink, int verbose, const char *message, size_t length) {
  auto *collector = static_cast<LogCollector *>(sink->opaque);
  std::lock_guard<std::mutex> lock(collector->mu);
  collector->last_verbose = verbose;
  collector->messages.emplace_back(message, length);
}

extern "C" void
FlushInsideSinkLevelLog(const nei_log_sink_st *sink, nei_log_level_e level, const char *message, size_t length) {
  (void)level;
  (void)message;
  (void)length;
  auto *collector = static_cast<LogCollector *>(sink->opaque);
  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector->mu);
  collector->messages.emplace_back(message, length);
}

extern "C" void
FlushInsideSinkVerboseLog(const nei_log_sink_st *sink, int verbose, const char *message, size_t length) {
  (void)message;
  (void)length;
  auto *collector = static_cast<LogCollector *>(sink->opaque);
  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector->mu);
  collector->last_verbose = verbose;
  collector->messages.emplace_back(message, length);
}

struct DefaultConfigSinkGuard {
  nei_log_sink_st *saved[NEI_LOG_MAX_SINKS_OF_CONFIG]{};

  DefaultConfigSinkGuard() {
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

  ~DefaultConfigSinkGuard() {
    nei_log_config_st *cfg = nei_log_default_config();
    for (size_t i = 0; i < NEI_LOG_MAX_SINKS_OF_CONFIG; ++i) {
      cfg->sinks[i] = saved[i];
    }
  }

  DefaultConfigSinkGuard(const DefaultConfigSinkGuard &) = delete;
  DefaultConfigSinkGuard &operator=(const DefaultConfigSinkGuard &) = delete;
};

#if !defined(_WIN32)
static void AppendCodepointAsUtf8(std::string *out, char32_t cp) {
  if (out == nullptr || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
    return;
  }
  if (cp <= 0x7Fu) {
    out->push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FFu) {
    out->push_back(static_cast<char>(0xC0u | ((cp >> 6) & 0x1Fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0xFFFFu) {
    out->push_back(static_cast<char>(0xE0u | ((cp >> 12) & 0x0Fu)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else {
    out->push_back(static_cast<char>(0xF0u | ((cp >> 18) & 0x07u)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}
#endif

/** Matches log.c non-Windows path: UTF-8 from native wchar_t (WCHAR_T / Unicode scalar values). */
static std::string ExpectedMbFromWideForLogCTest(const wchar_t *ws) {
  if (ws == nullptr) {
    return "(null)";
  }
#if defined(_WIN32)
  const int nchars = static_cast<int>(std::wcslen(ws));
  if (nchars == 0) {
    return {};
  }
  const int nb = WideCharToMultiByte(CP_ACP, 0, ws, nchars, nullptr, 0, nullptr, nullptr);
  if (nb <= 0) {
    return "[encoding error]";
  }
  std::string out(static_cast<size_t>(nb), '\0');
  const int wr = WideCharToMultiByte(CP_ACP, 0, ws, nchars, out.data(), nb, nullptr, nullptr);
  if (wr <= 0) {
    return "[encoding error]";
  }
  out.resize(static_cast<size_t>(wr));
  return out;
#else
  std::string out;
  while (*ws != L'\0') {
    char32_t cp = static_cast<char32_t>(*ws++);
    if (sizeof(wchar_t) == 2u && cp >= 0xD800u && cp <= 0xDBFFu && *ws >= 0xDC00u && *ws <= 0xDFFFu) {
      const char32_t lo = static_cast<char32_t>(*ws++);
      cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
    }
    AppendCodepointAsUtf8(&out, cp);
  }
  return out;
#endif
}

static std::string CurrentTestExecutablePath() {
#if defined(_WIN32)
  char path[MAX_PATH] = {0};
  const DWORD n = GetModuleFileNameA(NULL, path, static_cast<DWORD>(sizeof(path)));
  if (n == 0U || n >= static_cast<DWORD>(sizeof(path))) {
    return std::string();
  }
  return std::string(path, static_cast<size_t>(n));
#else
  char path[4096] = {0};
  const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1U);
  if (n <= 0) {
    return std::string();
  }
  return std::string(path, static_cast<size_t>(n));
#endif
}
} // namespace

TEST(LogCTest, FlushFromSinkCallbackDoesNotDeadlock) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = FlushInsideSinkLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  nei_llog(cfg_handle, NEI_L_INFO, __FILE__, __LINE__, "flush-in-sink", "first=%d", 1);
  nei_llog(cfg_handle, NEI_L_INFO, __FILE__, __LINE__, "flush-in-sink", "second=%d", 2);
  nei_log_flush();
  {
    std::lock_guard<std::mutex> lock(collector.mu);
    ASSERT_EQ(collector.messages.size(), 2U);
    EXPECT_NE(collector.messages[0].find("first=1"), std::string::npos);
    EXPECT_NE(collector.messages[1].find("second=2"), std::string::npos);
  }
  nei_log_remove_config(cfg_handle);

  collector.messages.clear();
  sink.llog = nullptr;
  sink.vlog = FlushInsideSinkVerboseLog;
  config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);
  nei_vlog(cfg_handle, 1, __FILE__, __LINE__, "flush-in-sink-v", "v=%d", 3);
  nei_log_flush();
  {
    std::lock_guard<std::mutex> lock(collector.mu);
    ASSERT_EQ(collector.messages.size(), 1U);
    EXPECT_EQ(collector.last_verbose, 1);
    EXPECT_NE(collector.messages[0].find("v=3"), std::string::npos);
  }
  nei_log_remove_config(cfg_handle);
}

TEST(LogCTest, ConcurrentFirstUseInitializationIsSafe) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);
  const uint32_t init_count_before = nei_log_get_runtime_init_count_for_test();

  constexpr int kThreadCount = 128;
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(kThreadCount));

  for (int i = 0; i < kThreadCount; ++i) {
    workers.emplace_back([&ready, &start, cfg_handle, i]() {
      ready.fetch_add(1, std::memory_order_relaxed);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      nei_llog_literal(cfg_handle, NEI_L_INFO, __FILE__, __LINE__, "concurrent-init", "x", 1U);
    });
  }

  while (ready.load(std::memory_order_acquire) != kThreadCount) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);

  for (auto &t : workers) {
    t.join();
  }

  nei_log_flush();
  {
    std::lock_guard<std::mutex> lock(collector.mu);
    ASSERT_EQ(collector.messages.size(), static_cast<size_t>(kThreadCount));
    for (const auto &msg : collector.messages) {
      EXPECT_NE(msg.find("x"), std::string::npos);
    }
  }
  EXPECT_EQ(nei_log_get_runtime_init_count_for_test(), (init_count_before == 0U) ? 1U : init_count_before);

  nei_log_remove_config(cfg_handle);
}

TEST(LogCTest, ConcurrentFirstUseInitializationStress) {
  const std::string exe = CurrentTestExecutablePath();
  ASSERT_FALSE(exe.empty());

  constexpr int kRounds = 16;
  for (int i = 0; i < kRounds; ++i) {
    const std::string cmd =
        "\"" + exe + "\" --gtest_filter=LogCTest.ConcurrentFirstUseInitializationIsSafe --gtest_brief=1";
    const int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0) << "round=" << i;
  }
}

TEST(LogCTest, AsyncPipelineDeepCopiesStringPayload) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  char mutable_text[64] = "first-message";
  nei_llog(cfg_handle, NEI_L_INFO, __FILE__, __LINE__, "test-func", "value=%s", mutable_text);
  std::memcpy(mutable_text, "mutated-after-log", sizeof("mutated-after-log"));

  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  ASSERT_FALSE(collector.messages.empty());
  EXPECT_NE(collector.messages[0].find("value=first-message"), std::string::npos);
  EXPECT_EQ(collector.messages[0].find("mutated-after-log"), std::string::npos);
  nei_log_remove_config(cfg_handle);
}

TEST(LogCTest, LiteralApisPassMessageWithoutPrintfExpansion) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  const char raw[] = "user fmt: %d %% not expanded";
  nei_llog_literal(cfg_handle, NEI_L_INFO, "lit.c", 5, "f", raw, sizeof(raw) - 1U);
  nei_log_flush();
  {
    std::lock_guard<std::mutex> lock(collector.mu);
    ASSERT_EQ(collector.messages.size(), 1U);
    EXPECT_NE(collector.messages[0].find("user fmt: %d %% not expanded"), std::string::npos);
  }
  collector.messages.clear();

  sink.llog = nullptr;
  sink.vlog = CollectVerboseLog;
  nei_vlog_literal(cfg_handle, 3, "lit.c", 6, "g", "verbose-bodyXX", 12U);
  nei_log_flush();
  {
    std::lock_guard<std::mutex> lock(collector.mu);
    ASSERT_EQ(collector.messages.size(), 1U);
    EXPECT_EQ(collector.last_verbose, 3);
    EXPECT_NE(collector.messages[0].find("verbose-body"), std::string::npos);
    EXPECT_EQ(collector.messages[0].find("verbose-bodyXX"), std::string::npos);
  }
  nei_log_remove_config(cfg_handle);
}

TEST(LogCTest, LiteralApi_EmptyAndNullMessageEmitPrefixOnly) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  nei_llog_literal(cfg_handle, NEI_L_INFO, "empty.c", 10, "fn", "", 0);
  nei_llog_literal(cfg_handle, NEI_L_WARN, "empty.c", 11, "fn", nullptr, 100);
  nei_log_flush();
  {
    std::lock_guard<std::mutex> lock(collector.mu);
    ASSERT_EQ(collector.messages.size(), 2U);
    EXPECT_NE(collector.messages[0].find("[I]"), std::string::npos);
    EXPECT_NE(collector.messages[0].find("empty.c:10"), std::string::npos);
    EXPECT_NE(collector.messages[1].find("[W]"), std::string::npos);
    EXPECT_NE(collector.messages[1].find("empty.c:11"), std::string::npos);
  }

  collector.messages.clear();
  sink.llog = nullptr;
  sink.vlog = CollectVerboseLog;
  nei_vlog_literal(cfg_handle, 2, "empty.c", 12, "fn", "", 0);
  nei_log_flush();
  {
    std::lock_guard<std::mutex> lock(collector.mu);
    ASSERT_EQ(collector.messages.size(), 1U);
    EXPECT_EQ(collector.last_verbose, 2);
    EXPECT_NE(collector.messages[0].find("empty.c:12"), std::string::npos);
  }
  nei_log_remove_config(cfg_handle);
}

TEST(LogCTest, LiteralApi_IncludesFileLineAndBodyInFormattedOutput) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  nei_llog_literal(cfg_handle, NEI_L_WARN, "unit_literal.c", 77, "lf", "payload-bytes", 13);
  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  const std::string &msg = collector.messages[0];
  EXPECT_NE(msg.find("[W]"), std::string::npos);
  EXPECT_NE(msg.find("unit_literal.c:77"), std::string::npos);
  EXPECT_NE(msg.find("payload-bytes"), std::string::npos);
  nei_log_remove_config(cfg_handle);
}

TEST(LogCTest, LiteralApi_RespectsLevelAndVerboseFilters) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.vlog = CollectVerboseLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.level_flags.all = 0xFFFFFFFFu & ~((uint32_t)1U << (uint32_t)NEI_L_INFO);
  config.verbose_threshold = 2;
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  nei_llog_literal(cfg_handle, NEI_L_INFO, "f.c", 1, "x", "no", 2);
  nei_llog_literal(cfg_handle, NEI_L_WARN, "f.c", 2, "x", "yes", 3);
  nei_vlog_literal(cfg_handle, 5, "f.c", 3, "x", "v-no", 4);
  nei_vlog_literal(cfg_handle, 1, "f.c", 4, "x", "v-yes", 5);
  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 2U);
  EXPECT_NE(collector.messages[0].find("[W]"), std::string::npos);
  EXPECT_NE(collector.messages[0].find("yes"), std::string::npos);
  EXPECT_EQ(collector.last_verbose, 1);
  EXPECT_NE(collector.messages[1].find("v-yes"), std::string::npos);
  nei_log_remove_config(cfg_handle);
}

TEST(LogCTest, LogThreadIdPrefixWhenConfigEnabled) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.log_thread_id = 1;
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  nei_llog(cfg_handle, NEI_L_INFO, __FILE__, __LINE__, "tid-test", "body=%d", 1);
  nei_log_flush();
  {
    std::lock_guard<std::mutex> lock(collector.mu);
    ASSERT_EQ(collector.messages.size(), 1U);
    EXPECT_NE(collector.messages[0].find("tid="), std::string::npos);
    EXPECT_NE(collector.messages[0].find("body=1"), std::string::npos);
  }
  nei_log_remove_config(cfg_handle);

  collector.messages.clear();
  sink.llog = nullptr;
  sink.vlog = CollectVerboseLog;
  config = *nei_log_default_config();
  config.log_thread_id = 1;
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);
  nei_vlog(cfg_handle, 2, __FILE__, __LINE__, "tid-v", "vb=%d", 3);
  nei_log_flush();
  {
    std::lock_guard<std::mutex> lock(collector.mu);
    ASSERT_EQ(collector.messages.size(), 1U);
    EXPECT_EQ(collector.last_verbose, 2);
    EXPECT_NE(collector.messages[0].find("tid="), std::string::npos);
    EXPECT_NE(collector.messages[0].find("vb=3"), std::string::npos);
  }

  nei_log_remove_config(cfg_handle);
}

TEST(LogCTest, LogThreadIdCacheInvalidatesOnConfigSlotReuse) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.log_thread_id = 1;
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;

  nei_log_config_handle_t first_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &first_handle), 0);
  nei_llog(first_handle, NEI_L_INFO, __FILE__, __LINE__, "tid-cache", "phase=%d", 1);
  nei_log_flush();
  {
    std::lock_guard<std::mutex> lock(collector.mu);
    ASSERT_EQ(collector.messages.size(), 1U);
    EXPECT_NE(collector.messages[0].find("tid="), std::string::npos);
  }
  nei_log_remove_config(first_handle);

  collector.messages.clear();
  config = *nei_log_default_config();
  config.log_thread_id = 0;
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;

  nei_log_config_handle_t second_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &second_handle), 0);
  ASSERT_EQ(first_handle, second_handle);

  nei_llog(second_handle, NEI_L_INFO, __FILE__, __LINE__, "tid-cache", "phase=%d", 2);
  nei_log_flush();
  {
    std::lock_guard<std::mutex> lock(collector.mu);
    ASSERT_EQ(collector.messages.size(), 1U);
    EXPECT_EQ(collector.messages[0].find("tid="), std::string::npos);
    EXPECT_NE(collector.messages[0].find("phase=2"), std::string::npos);
  }

  nei_log_remove_config(second_handle);
}

TEST(LogCTest, AsyncPipelineFormatsTimestampAndFileLineAfterMessageByDefault) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  nei_llog(cfg_handle, NEI_L_WARN, "unit_test_file.c", 123, "test-func", "answer=%d", 42);

  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  ASSERT_FALSE(collector.messages.empty());
  const std::string &msg = collector.messages[0];
  const size_t answer_pos = msg.find("answer=42");
  const size_t file_pos = msg.find("unit_test_file.c:123");
  EXPECT_TRUE(!msg.empty() && msg.front() == '[');
  EXPECT_NE(msg.find("[W]"), std::string::npos);
  EXPECT_NE(file_pos, std::string::npos);
  EXPECT_NE(answer_pos, std::string::npos);
  EXPECT_LT(answer_pos, file_pos);
  nei_log_remove_config(cfg_handle);
}

TEST(LogCTest, LogLocationCanBePlacedBeforeMessage) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.log_location_after_message = 0;
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  nei_llog(cfg_handle, NEI_L_WARN, "unit_test_file.c", 123, "test-func", "answer=%d", 42);

  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  const std::string &msg = collector.messages[0];
  const size_t answer_pos = msg.find("answer=42");
  const size_t file_pos = msg.find("unit_test_file.c:123");
  EXPECT_NE(file_pos, std::string::npos);
  EXPECT_NE(answer_pos, std::string::npos);
  EXPECT_LT(file_pos, answer_pos);
  nei_log_remove_config(cfg_handle);
}

TEST(LogCTest, LogLocationDisabledOmitsFileLineAndFunction) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.log_location = 0;
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  nei_llog(cfg_handle, NEI_L_INFO, "hide_file.c", 99, "hidden-func", "payload=%d", 7);
  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  const std::string &msg = collector.messages[0];
  EXPECT_NE(msg.find("[I]"), std::string::npos);
  EXPECT_NE(msg.find("payload=7"), std::string::npos);
  EXPECT_EQ(msg.find("hide_file.c:99"), std::string::npos);
  EXPECT_EQ(msg.find("hidden-func - "), std::string::npos);
  nei_log_remove_config(cfg_handle);
}

TEST(LogCTest, AsyncPipelineHonorsWidthPrecisionAndConfigFlags) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.short_level_tag = 0;
  config.short_path = 1;
  config.timestamp_style = NEI_LOG_TIMESTAMP_STYLE_ISO8601_MS;
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  nei_llog(cfg_handle,
           NEI_L_ERROR,
           "dir/subdir/test_path.c",
           77,
           "test-func",
           "x=%04d pi=%.2f dyn=%*.*f",
           7,
           3.14159,
           8,
           3,
           3.14159);

  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  ASSERT_FALSE(collector.messages.empty());
  const std::string &msg = collector.messages[0];
  const size_t dyn_pos = msg.find("dyn=   3.142");
  const size_t file_pos = msg.find("test_path.c:77");
  EXPECT_NE(msg.find("[ERROR]"), std::string::npos);
  EXPECT_EQ(msg.find("dir/subdir/test_path.c:77"), std::string::npos);
  EXPECT_NE(file_pos, std::string::npos);
  EXPECT_NE(msg.find("test-func"), std::string::npos);
  EXPECT_NE(msg.find("x=0007"), std::string::npos);
  EXPECT_NE(msg.find("pi=3.14"), std::string::npos);
  EXPECT_NE(dyn_pos, std::string::npos);
  EXPECT_LT(dyn_pos, file_pos);
  nei_log_remove_config(cfg_handle);
}

TEST(LogCTest, AsyncPipelineLengthModifiersLdJzLf) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  const long lv = 99;
  const intmax_t jv = 7;
  const size_t zv = 8;
  const long double ldv = static_cast<long double>(2.5);

  nei_llog(cfg_handle, NEI_L_DEBUG, "fmt_test.c", 1, "fn", "ld=%ld jd=%jd zu=%zu Lf=%.3Lf", lv, jv, zv, ldv);

  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  ASSERT_FALSE(collector.messages.empty());
  const std::string &msg = collector.messages[0];
  EXPECT_NE(msg.find("ld=99"), std::string::npos);
  EXPECT_NE(msg.find("jd=7"), std::string::npos);
  EXPECT_NE(msg.find("zu=8"), std::string::npos);
  EXPECT_NE(msg.find("Lf=2.500"), std::string::npos);
  nei_log_remove_config(cfg_handle);
}

TEST(LogCTest, DirectLlogWithDefaultConfigUsesSink) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  DefaultConfigSinkGuard guard;
  guard.set_primary_sink(&sink);

  nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_DEBUG, __FILE__, __LINE__, "direct-fn", "x=%d", 1);

  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  ASSERT_FALSE(collector.messages.empty());
  EXPECT_NE(collector.messages[0].find("[D]"), std::string::npos);
  EXPECT_NE(collector.messages[0].find("x=1"), std::string::npos);
}

TEST(LogCTest, MacrosUseDefaultConfigAndEmitLevelTags) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  DefaultConfigSinkGuard guard;
  guard.set_primary_sink(&sink);

  NEI_LOG_TRACE("trace=%d", 1);
  NEI_LOG_DEBUG("debug=%s", "d");
  NEI_LOG_INFO("info-only");
  NEI_LOG_WARN("warn=%u", 3U);
  NEI_LOG_ERROR("err");
  NEI_LOG_FATAL("fatal=%x", 0xab);
  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 6U);

  EXPECT_NE(collector.messages[0].find("[T]"), std::string::npos);
  EXPECT_NE(collector.messages[0].find("trace=1"), std::string::npos);

  EXPECT_NE(collector.messages[1].find("[D]"), std::string::npos);
  EXPECT_NE(collector.messages[1].find("debug=d"), std::string::npos);

  EXPECT_NE(collector.messages[2].find("[I]"), std::string::npos);
  EXPECT_NE(collector.messages[2].find("info-only"), std::string::npos);

  EXPECT_NE(collector.messages[3].find("[W]"), std::string::npos);
  EXPECT_NE(collector.messages[3].find("warn=3"), std::string::npos);

  EXPECT_NE(collector.messages[4].find("[E]"), std::string::npos);
  EXPECT_NE(collector.messages[4].find("err"), std::string::npos);

  EXPECT_NE(collector.messages[5].find("[F]"), std::string::npos);
  EXPECT_NE(collector.messages[5].find("fatal=ab"), std::string::npos);
}

TEST(LogCTest, GenericLevelMacroUsesProvidedLevel) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  DefaultConfigSinkGuard guard;
  guard.set_primary_sink(&sink);

  NEI_LOG(NEI_L_WARN, "generic=%d", 7);
  nei_log_flush();

  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  EXPECT_NE(collector.messages[0].find("[W]"), std::string::npos);
  EXPECT_NE(collector.messages[0].find("generic=7"), std::string::npos);
}

TEST(LogCTest, ConditionalLevelMacrosRespectConditionAndEvaluateOnce) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  DefaultConfigSinkGuard guard;
  guard.set_primary_sink(&sink);

  int cond_eval_count = 0;
  NEI_LOG_IF((++cond_eval_count) == 1, NEI_L_INFO, "if-generic-true");
  NEI_LOG_IF((++cond_eval_count) == 100, NEI_L_INFO, "if-generic-false");

  int info_if_eval_count = 0;
  NEI_LOG_INFO_IF((++info_if_eval_count) == 1, "if-info-true");
  NEI_LOG_INFO_IF((++info_if_eval_count) == 100, "if-info-false");

  nei_log_flush();

  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(cond_eval_count, 2);
  ASSERT_EQ(info_if_eval_count, 2);
  ASSERT_EQ(collector.messages.size(), 2U);
  EXPECT_NE(collector.messages[0].find("if-generic-true"), std::string::npos);
  EXPECT_NE(collector.messages[1].find("if-info-true"), std::string::npos);
  EXPECT_EQ(collector.messages[0].find("if-generic-false"), std::string::npos);
  EXPECT_EQ(collector.messages[1].find("if-info-false"), std::string::npos);
}

TEST(LogCTest, VerboseMacroUsesVlogAndVerboseLevel) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.vlog = CollectVerboseLog;
  sink.opaque = &collector;

  DefaultConfigSinkGuard guard;
  guard.set_primary_sink(&sink);

  NEI_LOG_VERBOSE(7, "verb=%d", 42);

  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  ASSERT_FALSE(collector.messages.empty());
  EXPECT_EQ(collector.last_verbose, 7);
  EXPECT_NE(collector.messages[0].find("[V]"), std::string::npos);
  EXPECT_NE(collector.messages[0].find("verb=42"), std::string::npos);
}

TEST(LogCTest, ConditionalVerboseMacroRespectsConditionAndVerboseLevel) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.vlog = CollectVerboseLog;
  sink.opaque = &collector;

  DefaultConfigSinkGuard guard;
  guard.set_primary_sink(&sink);

  int cond_eval_count = 0;
  NEI_LOG_VERBOSE_IF((++cond_eval_count) == 1, 5, "verb-if-true=%d", 1);
  NEI_LOG_VERBOSE_IF((++cond_eval_count) == 100, 6, "verb-if-false=%d", 2);

  nei_log_flush();

  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(cond_eval_count, 2);
  ASSERT_EQ(collector.messages.size(), 1U);
  EXPECT_EQ(collector.last_verbose, 5);
  EXPECT_NE(collector.messages[0].find("verb-if-true=1"), std::string::npos);
  EXPECT_EQ(collector.messages[0].find("verb-if-false=2"), std::string::npos);
}

TEST(LogCTest, AsyncBurstThenFlushDeliversEveryMessage) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  DefaultConfigSinkGuard guard;
  guard.set_primary_sink(&sink);

  constexpr int kCount = 200;
  for (int i = 0; i < kCount; ++i) {
    nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_L_INFO, __FILE__, __LINE__, "burst", "burst id=%d", i);
  }
  nei_log_flush();

  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), static_cast<size_t>(kCount));
  for (int i = 0; i < kCount; ++i) {
    const std::string needle = "burst id=" + std::to_string(i);
    EXPECT_NE(collector.messages[static_cast<size_t>(i)].find(needle), std::string::npos)
        << "missing or reordered message for i=" << i;
  }
}

TEST(LogCTest, ConfigAddGetByIdAndEmit) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st cfg = *nei_log_default_config();
  cfg.sinks[0] = &sink;
  cfg.sinks[1] = nullptr;

  nei_log_config_handle_t handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  EXPECT_EQ(nei_log_add_config(&cfg, &handle), 0);
  ASSERT_NE(handle, NEI_LOG_INVALID_CONFIG_HANDLE);

  const nei_log_config_st *got = nei_log_get_config(handle);
  ASSERT_NE(got, nullptr);

  nei_llog(handle, NEI_L_INFO, __FILE__, __LINE__, "config-test", "hello=%d", 42);

  nei_log_flush();

  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  EXPECT_NE(collector.messages[0].find("[I]"), std::string::npos);
  EXPECT_NE(collector.messages[0].find("hello=42"), std::string::npos);

  nei_log_remove_config(handle);
}

TEST(LogCTest, ConfigRemoveByIdReturnsNull) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st cfg = *nei_log_default_config();
  cfg.sinks[0] = &sink;
  cfg.sinks[1] = nullptr;

  nei_log_config_handle_t handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  EXPECT_EQ(nei_log_add_config(&cfg, &handle), 0);

  EXPECT_NE(nei_log_get_config(handle), nullptr);
  nei_log_remove_config(handle);
  EXPECT_EQ(nei_log_get_config(handle), nullptr);
}

TEST(LogCTest, ConfigHandleApiAddGetRemoveWorks) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st cfg = *nei_log_default_config();
  cfg.sinks[0] = &sink;
  cfg.sinks[1] = nullptr;

  nei_log_config_handle_t handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  EXPECT_EQ(nei_log_add_config(&cfg, &handle), 0);
  EXPECT_NE(handle, NEI_LOG_INVALID_CONFIG_HANDLE);
  EXPECT_NE(nei_log_get_config(handle), nullptr);

  nei_llog(handle, NEI_L_INFO, __FILE__, __LINE__, "cfg-handle", "hello=%d", 7);
  nei_log_flush();
  {
    std::lock_guard<std::mutex> lock(collector.mu);
    ASSERT_EQ(collector.messages.size(), 1U);
    EXPECT_NE(collector.messages[0].find("hello=7"), std::string::npos);
  }

  nei_log_remove_config(handle);
  EXPECT_EQ(nei_log_get_config(handle), nullptr);
}

TEST(LogCTest, ConfigCapacityMax16IncludingDefault) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st cfg = *nei_log_default_config();
  cfg.sinks[0] = &sink;
  cfg.sinks[1] = nullptr;

  std::vector<nei_log_config_handle_t> handles;
  handles.reserve(16);

  // default config already occupies 1, so we can add up to 15 more.
  for (size_t i = 0; i < 15U; ++i) {
    nei_log_config_handle_t handle = NEI_LOG_INVALID_CONFIG_HANDLE;
    EXPECT_EQ(nei_log_add_config(&cfg, &handle), 0);
    handles.emplace_back(handle);
  }

  // Next add should fail.
  EXPECT_LT(nei_log_add_config(&cfg, nullptr), 0);

  // Cleanup so other tests won't observe a full table.
  for (const auto &handle : handles) {
    nei_log_remove_config(handle);
  }
}

TEST(LogCTest, BuiltinFileSinkOwnsFileHandleLifecycle) {
  char tmp_name[L_tmpnam] = {};
#if defined(_WIN32)
  ASSERT_EQ(tmpnam_s(tmp_name, L_tmpnam), 0);
#else
  ASSERT_NE(std::tmpnam(tmp_name), nullptr);
#endif
  const std::string file_path = std::string(tmp_name) + ".log";
  (void)std::remove(file_path.c_str());

  nei_log_sink_st *sink = nei_log_create_default_file_sink(file_path.c_str(), NULL);
  ASSERT_NE(sink, nullptr);

  nei_log_config_st cfg = *nei_log_default_config();
  cfg.log_to_console = 0;
  cfg.level_flags = {};
  cfg.level_flags.flags.info = 1U;
  cfg.verbose_threshold = 2;
  cfg.sinks[0] = sink;
  cfg.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&cfg, &cfg_handle), 0);

  nei_llog(cfg_handle, NEI_L_INFO, __FILE__, __LINE__, "file-sink", "info=%d", 11);
  nei_llog(cfg_handle, NEI_L_DEBUG, __FILE__, __LINE__, "file-sink", "debug=%d", 22);
  nei_vlog(cfg_handle, 1, __FILE__, __LINE__, "file-sink", "verbose=%d", 33);
  nei_vlog(cfg_handle, 4, __FILE__, __LINE__, "file-sink", "verbose_drop=%d", 44);
  nei_log_flush();

  nei_log_remove_config(cfg_handle);
  nei_log_destroy_sink(sink);

  std::ifstream in(file_path, std::ios::binary);
  ASSERT_TRUE(in.good());
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(content.find("info=11"), std::string::npos);
  EXPECT_NE(content.find("verbose=33"), std::string::npos);
  EXPECT_EQ(content.find("debug=22"), std::string::npos);
  EXPECT_EQ(content.find("verbose_drop=44"), std::string::npos);

  (void)std::remove(file_path.c_str());
}

TEST(LogCTest, BuiltinFileSinkRotatesAtConfiguredSize) {
  char tmp_name[L_tmpnam] = {};
#if defined(_WIN32)
  ASSERT_EQ(tmpnam_s(tmp_name, L_tmpnam), 0);
#else
  ASSERT_NE(std::tmpnam(tmp_name), nullptr);
#endif
  const std::string file_path = std::string(tmp_name) + ".log";
  const std::string file_path_1 = file_path + ".1";
  const std::string file_path_2 = file_path + ".2";
  (void)std::remove(file_path.c_str());
  (void)std::remove(file_path_1.c_str());
  (void)std::remove(file_path_2.c_str());

  nei_log_default_file_sink_options_st rotate_opts = nei_log_default_file_sink_options();
  rotate_opts.max_file_bytes   = 64U;
  rotate_opts.max_backup_files = 2U;
  nei_log_sink_st *sink = nei_log_create_default_file_sink(file_path.c_str(), &rotate_opts);
  ASSERT_NE(sink, nullptr);

  nei_log_config_st cfg = *nei_log_default_config();
  cfg.log_to_console = 0;
  cfg.level_flags = {};
  cfg.level_flags.flags.info = 1U;
  cfg.timestamp_style = NEI_LOG_TIMESTAMP_STYLE_NONE;
  cfg.short_level_tag = 0U;
  cfg.log_location = 0U;
  cfg.log_thread_id = 0U;
  cfg.sinks[0] = sink;
  cfg.sinks[1] = nullptr;

  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&cfg, &cfg_handle), 0);

  nei_llog(cfg_handle, NEI_L_INFO, __FILE__, __LINE__, "rotate", "entry-111111111111111111111111111111");
  nei_llog(cfg_handle, NEI_L_INFO, __FILE__, __LINE__, "rotate", "entry-222222222222222222222222222222");
  nei_llog(cfg_handle, NEI_L_INFO, __FILE__, __LINE__, "rotate", "entry-333333333333333333333333333333");
  nei_log_flush();

  nei_log_remove_config(cfg_handle);
  nei_log_destroy_sink(sink);

  std::ifstream current(file_path, std::ios::binary);
  std::ifstream rotated1(file_path_1, std::ios::binary);
  std::ifstream rotated2(file_path_2, std::ios::binary);
  ASSERT_TRUE(current.good());
  ASSERT_TRUE(rotated1.good());
  ASSERT_TRUE(rotated2.good());

  const std::string current_content((std::istreambuf_iterator<char>(current)), std::istreambuf_iterator<char>());
  const std::string rotated1_content((std::istreambuf_iterator<char>(rotated1)), std::istreambuf_iterator<char>());
  const std::string rotated2_content((std::istreambuf_iterator<char>(rotated2)), std::istreambuf_iterator<char>());

  EXPECT_NE(current_content.find("entry-333333333333333333333333333333"), std::string::npos);
  EXPECT_NE(rotated1_content.find("entry-222222222222222222222222222222"), std::string::npos);
  EXPECT_NE(rotated2_content.find("entry-111111111111111111111111111111"), std::string::npos);

  (void)std::remove(file_path.c_str());
  (void)std::remove(file_path_1.c_str());
  (void)std::remove(file_path_2.c_str());
}

TEST(LogCTest, ConfigAddRemoveIsThreadSafeAtRuntime) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st cfg = *nei_log_default_config();
  cfg.log_to_console = 0;
  cfg.sinks[0] = &sink;
  cfg.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&cfg, &cfg_handle), 0);
  std::atomic<nei_log_config_handle_t> active_handle{cfg_handle};

  std::atomic<int> stop{0};
  std::thread producer([&]() {
    int i = 0;
    while (stop.load(std::memory_order_relaxed) == 0) {
      const nei_log_config_handle_t h = active_handle.load(std::memory_order_relaxed);
      if (h != NEI_LOG_INVALID_CONFIG_HANDLE) {
        nei_llog(h, NEI_L_INFO, __FILE__, __LINE__, "race", "runtime=%d", i);
      }
      ++i;
    }
  });

  std::thread manager([&]() {
    for (int i = 0; i < 200; ++i) {
      if ((i % 2) == 1) {
        const nei_log_config_handle_t old_h = active_handle.exchange(NEI_LOG_INVALID_CONFIG_HANDLE);
        if (old_h != NEI_LOG_INVALID_CONFIG_HANDLE) {
          nei_log_remove_config(old_h);
        }
        nei_log_config_handle_t new_h = NEI_LOG_INVALID_CONFIG_HANDLE;
        (void)nei_log_add_config(&cfg, &new_h);
        active_handle.store(new_h, std::memory_order_relaxed);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(1, std::memory_order_relaxed);
  });

  manager.join();
  producer.join();
  nei_log_flush();
  {
    const nei_log_config_handle_t h = active_handle.load(std::memory_order_relaxed);
    if (h != NEI_LOG_INVALID_CONFIG_HANDLE) {
      nei_log_remove_config(h);
    }
  }

  std::lock_guard<std::mutex> lock(collector.mu);
  EXPECT_GT(collector.messages.size(), 0U);
}

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#endif

TEST(LogCTest, WsFormat_NullUsesPlaceholder) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  nei_llog(cfg_handle, NEI_L_INFO, __FILE__, __LINE__, "ws-null", "wide=%ls!", static_cast<const wchar_t *>(nullptr));
  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  EXPECT_NE(collector.messages[0].find("wide=(null)!"), std::string::npos);
  nei_log_remove_config(cfg_handle);
}

TEST(LogCTest, WsFormat_EmptyWideString) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  static const wchar_t kEmpty[] = {L'\0'};
  nei_llog(cfg_handle, NEI_L_INFO, __FILE__, __LINE__, "ws-empty", "wide=%ls|end", kEmpty);
  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  EXPECT_NE(collector.messages[0].find("wide=|end"), std::string::npos);
  nei_log_remove_config(cfg_handle);
}

TEST(LogCTest, WsFormat_CjkHiraganaKatakanaHangulAndExtensionB) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  static const wchar_t kWide[] = L"\u4E2D\u6587"
                                 L"\u3042\u3044"
                                 L"\u30A2\u30FC"
                                 L"\uD55C\uAE00"
                                 L"\U00020000";

  const std::string expected_body = ExpectedMbFromWideForLogCTest(kWide);
  nei_llog(cfg_handle, NEI_L_INFO, __FILE__, __LINE__, "ws-cjk", "payload=%ls!", kWide);
  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  const std::string needle = "payload=" + expected_body + "!";
  EXPECT_NE(collector.messages[0].find(needle), std::string::npos) << "formatted=" << collector.messages[0];
  nei_log_remove_config(cfg_handle);
}

TEST(LogCTest, WsFormat_VlogSameCjkPayload) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.vlog = CollectVerboseLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  static const wchar_t kWide[] = L"\u97F3\u8AAD\u307F\U00024B62";
  const std::string expected_body = ExpectedMbFromWideForLogCTest(kWide);
  nei_vlog(cfg_handle, 1, __FILE__, __LINE__, "ws-v", "v=%ls.", kWide);
  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  EXPECT_EQ(collector.last_verbose, 1);
  const std::string needle = "v=" + expected_body + ".";
  EXPECT_NE(collector.messages[0].find(needle), std::string::npos) << "formatted=" << collector.messages[0];
  nei_log_remove_config(cfg_handle);
}

TEST(LogCTest, WsFormat_ConvertedPayloadNotAffectedByLaterWideBufferMutation) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  wchar_t mutable_wide[8] = {};
  mutable_wide[0] = L'\u4E2D';
  mutable_wide[1] = L'\u6587';
  mutable_wide[2] = L'\0';

  const std::string expected_before = ExpectedMbFromWideForLogCTest(mutable_wide);
  nei_llog(cfg_handle, NEI_L_INFO, __FILE__, __LINE__, "ws-mut", "w=%ls", mutable_wide);
  mutable_wide[0] = L'X';
  mutable_wide[1] = L'Y';
  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  EXPECT_NE(collector.messages[0].find("w=" + expected_before), std::string::npos) << collector.messages[0];
  nei_log_remove_config(cfg_handle);
}

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

TEST(LogCTest, ImmediateCrashOnFatalConfigurationOption) {
  /* Verify that the immediate_crash_on_fatal flag can be set and read correctly. */
  nei_log_config_st cfg = *nei_log_default_config();

  /* Initially should be disabled (0) by default */
  EXPECT_EQ(cfg.immediate_crash_on_fatal, 0U);

  /* Enable the flag */
  cfg.immediate_crash_on_fatal = 1U;
  EXPECT_EQ(cfg.immediate_crash_on_fatal, 1U);

  /* Disable it again */
  cfg.immediate_crash_on_fatal = 0U;
  EXPECT_EQ(cfg.immediate_crash_on_fatal, 0U);

  /* Verify that a custom config with the flag set persists */
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  cfg.immediate_crash_on_fatal = 1U;
  ASSERT_EQ(nei_log_add_config(&cfg, &cfg_handle), 0);

  nei_log_config_st *retrieved = nei_log_get_config(cfg_handle);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->immediate_crash_on_fatal, 1U);

  nei_log_remove_config(cfg_handle);

  /* Default config should still have it disabled after the above operations */
  nei_log_config_st *default_cfg = nei_log_default_config();
  EXPECT_EQ(default_cfg->immediate_crash_on_fatal, 0U);
}

TEST(LogCTest, InstallCrashHandlerIsIdempotent) {
  EXPECT_EQ(nei_log_install_crash_handler(NEI_LOG_INVALID_CONFIG_HANDLE), 0);
  EXPECT_EQ(nei_log_install_crash_handler(NEI_LOG_INVALID_CONFIG_HANDLE), 0);
}

TEST(LogCTest, ImmediateCrashOnFatalTriggersProcessExit) {
  /* This test uses EXPECT_DEATH to verify that emitting a FATAL log with
   * immediate_crash_on_fatal enabled actually crashes the process.
   * EXPECT_DEATH runs the code in a child process and verifies it exits abnormally. */
  EXPECT_DEATH({
    nei_log_config_st cfg = *nei_log_default_config();
    cfg.immediate_crash_on_fatal = 1U;
    cfg.log_to_console = 0U;
    cfg.level_flags.all = 0xFFFFFFFFu;  /* Enable all levels */

    nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
    if (nei_log_add_config(&cfg, &cfg_handle) != 0) {
      return;  /* Bail out if config add fails */
    }

    /* Emit a FATAL log - this should trigger immediate crash */
    nei_llog(cfg_handle, NEI_L_FATAL, __FILE__, __LINE__, "crash_test",
             "fatal error: immediate crash should be triggered now!");
    nei_log_flush();

    /* This line should never be reached */
    nei_log_remove_config(cfg_handle);
  }, ".*");  /* Expect any form of abnormal termination */
}

TEST(LogCTest, ImmediateCrashOnFatalFallbackPathTerminates) {
#if defined(_WIN32)
  EXPECT_EXIT({
    _putenv_s("NEI_LOG_TEST_SKIP_PRIMARY_CRASH", "1");
    _putenv_s("NEI_LOG_TEST_SKIP_SECONDARY_CRASH", "1");

    nei_log_config_st cfg = *nei_log_default_config();
    cfg.immediate_crash_on_fatal = 1U;
    cfg.log_to_console = 0U;
    cfg.level_flags.all = 0xFFFFFFFFu;

    nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
    if (nei_log_add_config(&cfg, &cfg_handle) != 0) {
      _Exit(3);
    }

    nei_llog(cfg_handle, NEI_L_FATAL, __FILE__, __LINE__, "crash_fallback", "fatal triggers fallback exit path");
    _Exit(4);
  }, ::testing::ExitedWithCode(0xEE), ".*");
#else
  EXPECT_EXIT({
    setenv("NEI_LOG_TEST_SKIP_PRIMARY_CRASH", "1", 1);
    setenv("NEI_LOG_TEST_SKIP_SECONDARY_CRASH", "1", 1);

    nei_log_config_st cfg = *nei_log_default_config();
    cfg.immediate_crash_on_fatal = 1U;
    cfg.log_to_console = 0U;
    cfg.level_flags.all = 0xFFFFFFFFu;

    nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
    if (nei_log_add_config(&cfg, &cfg_handle) != 0) {
      _Exit(3);
    }

    nei_llog(cfg_handle, NEI_L_FATAL, __FILE__, __LINE__, "crash_fallback", "fatal triggers fallback exit path");
    _Exit(4);
  }, ::testing::ExitedWithCode(134), ".*");
#endif
}
