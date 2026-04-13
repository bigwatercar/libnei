#include <gtest/gtest.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nei/log/log.h"

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

  nei_llog(cfg_handle, NEI_LOG_LEVEL_INFO, __FILE__, __LINE__, "flush-in-sink", "first=%d", 1);
  nei_llog(cfg_handle, NEI_LOG_LEVEL_INFO, __FILE__, __LINE__, "flush-in-sink", "second=%d", 2);
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
  nei_llog(cfg_handle, NEI_LOG_LEVEL_INFO, __FILE__, __LINE__, "test-func", "value=%s", mutable_text);
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
  nei_llog_literal(cfg_handle, NEI_LOG_LEVEL_INFO, "lit.c", 5, "f", raw, sizeof(raw) - 1U);
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

  nei_llog_literal(cfg_handle, NEI_LOG_LEVEL_INFO, "empty.c", 10, "fn", "", 0);
  nei_llog_literal(cfg_handle, NEI_LOG_LEVEL_WARN, "empty.c", 11, "fn", nullptr, 100);
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

  nei_llog_literal(cfg_handle, NEI_LOG_LEVEL_WARN, "unit_literal.c", 77, "lf", "payload-bytes", 13);
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
  config.level_flags.all = 0xFFFFFFFFu & ~((uint32_t)1U << (uint32_t)NEI_LOG_LEVEL_INFO);
  config.verbose_threshold = 2;
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  nei_llog_literal(cfg_handle, NEI_LOG_LEVEL_INFO, "f.c", 1, "x", "no", 2);
  nei_llog_literal(cfg_handle, NEI_LOG_LEVEL_WARN, "f.c", 2, "x", "yes", 3);
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

  nei_llog(cfg_handle, NEI_LOG_LEVEL_INFO, __FILE__, __LINE__, "tid-test", "body=%d", 1);
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

TEST(LogCTest, AsyncPipelineFormatsTimestampAndFileLinePrefix) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  nei_log_config_handle_t cfg_handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  ASSERT_EQ(nei_log_add_config(&config, &cfg_handle), 0);

  nei_llog(cfg_handle, NEI_LOG_LEVEL_WARN, "unit_test_file.c", 123, "test-func", "answer=%d", 42);

  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  ASSERT_FALSE(collector.messages.empty());
  const std::string &msg = collector.messages[0];
  EXPECT_TRUE(!msg.empty() && msg.front() == '[');
  EXPECT_NE(msg.find("[W]"), std::string::npos);
  EXPECT_NE(msg.find("unit_test_file.c:123"), std::string::npos);
  EXPECT_NE(msg.find("answer=42"), std::string::npos);
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
           NEI_LOG_LEVEL_ERROR,
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
  EXPECT_NE(msg.find("[ERROR]"), std::string::npos);
  EXPECT_EQ(msg.find("dir/subdir/test_path.c:77"), std::string::npos);
  EXPECT_NE(msg.find("test_path.c:77"), std::string::npos);
  EXPECT_NE(msg.find("test-func - "), std::string::npos);
  EXPECT_NE(msg.find("x=0007"), std::string::npos);
  EXPECT_NE(msg.find("pi=3.14"), std::string::npos);
  EXPECT_NE(msg.find("dyn=   3.142"), std::string::npos);
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

  nei_llog(cfg_handle, NEI_LOG_LEVEL_DEBUG, "fmt_test.c", 1, "fn", "ld=%ld jd=%jd zu=%zu Lf=%.3Lf", lv, jv, zv, ldv);

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

  nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_LOG_LEVEL_DEBUG, __FILE__, __LINE__, "direct-fn", "x=%d", 1);

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

TEST(LogCTest, AsyncBurstThenFlushDeliversEveryMessage) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  DefaultConfigSinkGuard guard;
  guard.set_primary_sink(&sink);

  constexpr int kCount = 200;
  for (int i = 0; i < kCount; ++i) {
    nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_LOG_LEVEL_INFO, __FILE__, __LINE__, "burst", "burst id=%d", i);
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

  nei_llog(handle, NEI_LOG_LEVEL_INFO, __FILE__, __LINE__, "config-test", "hello=%d", 42);

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

  nei_llog(handle, NEI_LOG_LEVEL_INFO, __FILE__, __LINE__, "cfg-handle", "hello=%d", 7);
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

  nei_log_sink_st *sink = nei_log_create_default_file_sink(file_path.c_str());
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

  nei_llog(cfg_handle, NEI_LOG_LEVEL_INFO, __FILE__, __LINE__, "file-sink", "info=%d", 11);
  nei_llog(cfg_handle, NEI_LOG_LEVEL_DEBUG, __FILE__, __LINE__, "file-sink", "debug=%d", 22);
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
        nei_llog(h, NEI_LOG_LEVEL_INFO, __FILE__, __LINE__, "race", "runtime=%d", i);
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
