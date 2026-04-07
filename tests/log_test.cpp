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

TEST(LogCTest, AsyncPipelineDeepCopiesStringPayload) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  const char cfg_id[] = "cfg-deepcpy";
  ASSERT_EQ(nei_log_add_config(cfg_id, &config), 0);

  char mutable_text[64] = "first-message";
  nei_llog(cfg_id, NEI_LOG_LEVEL_INFO, __FILE__, __LINE__, "test-func", "value=%s", mutable_text);
  std::memcpy(mutable_text, "mutated-after-log", sizeof("mutated-after-log"));

  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  ASSERT_FALSE(collector.messages.empty());
  EXPECT_NE(collector.messages[0].find("value=first-message"), std::string::npos);
  EXPECT_EQ(collector.messages[0].find("mutated-after-log"), std::string::npos);
  nei_log_remove_config(cfg_id);
}

TEST(LogCTest, LiteralApisPassMessageWithoutPrintfExpansion) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  const char cfg_id[] = "cfg-literal";
  ASSERT_EQ(nei_log_add_config(cfg_id, &config), 0);

  const char raw[] = "user fmt: %d %% not expanded";
  nei_llog_literal(cfg_id, NEI_LOG_LEVEL_INFO, "lit.c", 5, "f", raw, sizeof(raw) - 1U);
  nei_log_flush();
  {
    std::lock_guard<std::mutex> lock(collector.mu);
    ASSERT_EQ(collector.messages.size(), 1U);
    EXPECT_NE(collector.messages[0].find("user fmt: %d %% not expanded"), std::string::npos);
  }
  collector.messages.clear();

  sink.llog = nullptr;
  sink.vlog = CollectVerboseLog;
  nei_vlog_literal(cfg_id, 3, "lit.c", 6, "g", "verbose-bodyXX", 12U);
  nei_log_flush();
  {
    std::lock_guard<std::mutex> lock(collector.mu);
    ASSERT_EQ(collector.messages.size(), 1U);
    EXPECT_EQ(collector.last_verbose, 3);
    EXPECT_NE(collector.messages[0].find("verbose-body"), std::string::npos);
    EXPECT_EQ(collector.messages[0].find("verbose-bodyXX"), std::string::npos);
  }
  nei_log_remove_config(cfg_id);
}

TEST(LogCTest, LiteralApi_EmptyAndNullMessageEmitPrefixOnly) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  const char cfg_id[] = "cfg-lit-empty";
  ASSERT_EQ(nei_log_add_config(cfg_id, &config), 0);

  nei_llog_literal(cfg_id, NEI_LOG_LEVEL_INFO, "empty.c", 10, "fn", "", 0);
  nei_llog_literal(cfg_id, NEI_LOG_LEVEL_WARN, "empty.c", 11, "fn", nullptr, 100);
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
  nei_vlog_literal(cfg_id, 2, "empty.c", 12, "fn", "", 0);
  nei_log_flush();
  {
    std::lock_guard<std::mutex> lock(collector.mu);
    ASSERT_EQ(collector.messages.size(), 1U);
    EXPECT_EQ(collector.last_verbose, 2);
    EXPECT_NE(collector.messages[0].find("empty.c:12"), std::string::npos);
  }
  nei_log_remove_config(cfg_id);
}

TEST(LogCTest, LiteralApi_IncludesFileLineAndBodyInFormattedOutput) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  const char cfg_id[] = "cfg-lit-prefix";
  ASSERT_EQ(nei_log_add_config(cfg_id, &config), 0);

  nei_llog_literal(cfg_id, NEI_LOG_LEVEL_WARN, "unit_literal.c", 77, "lf", "payload-bytes", 13);
  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  const std::string &msg = collector.messages[0];
  EXPECT_NE(msg.find("[W]"), std::string::npos);
  EXPECT_NE(msg.find("unit_literal.c:77"), std::string::npos);
  EXPECT_NE(msg.find("payload-bytes"), std::string::npos);
  nei_log_remove_config(cfg_id);
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
  const char cfg_id[] = "cfg-lit-filter";
  ASSERT_EQ(nei_log_add_config(cfg_id, &config), 0);

  nei_llog_literal(cfg_id, NEI_LOG_LEVEL_INFO, "f.c", 1, "x", "no", 2);
  nei_llog_literal(cfg_id, NEI_LOG_LEVEL_WARN, "f.c", 2, "x", "yes", 3);
  nei_vlog_literal(cfg_id, 5, "f.c", 3, "x", "v-no", 4);
  nei_vlog_literal(cfg_id, 1, "f.c", 4, "x", "v-yes", 5);
  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 2U);
  EXPECT_NE(collector.messages[0].find("[W]"), std::string::npos);
  EXPECT_NE(collector.messages[0].find("yes"), std::string::npos);
  EXPECT_EQ(collector.last_verbose, 1);
  EXPECT_NE(collector.messages[1].find("v-yes"), std::string::npos);
  nei_log_remove_config(cfg_id);
}

TEST(LogCTest, AsyncPipelineFormatsTimestampAndFileLinePrefix) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  const char cfg_id[] = "cfg-prefix";
  ASSERT_EQ(nei_log_add_config(cfg_id, &config), 0);

  nei_llog(cfg_id, NEI_LOG_LEVEL_WARN, "unit_test_file.c", 123, "test-func", "answer=%d", 42);

  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  ASSERT_FALSE(collector.messages.empty());
  const std::string &msg = collector.messages[0];
  EXPECT_TRUE(!msg.empty() && msg.front() == '[');
  EXPECT_NE(msg.find("[W]"), std::string::npos);
  EXPECT_NE(msg.find("unit_test_file.c:123"), std::string::npos);
  EXPECT_NE(msg.find("answer=42"), std::string::npos);
  nei_log_remove_config(cfg_id);
}

TEST(LogCTest, AsyncPipelineHonorsWidthPrecisionAndConfigFlags) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.short_level_tag = 0;
  config.short_path = 1;
  config.datetime_format = "%Y/%m/%d %H:%M:%S";
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  const char cfg_id[] = "cfg-width";
  ASSERT_EQ(nei_log_add_config(cfg_id, &config), 0);

  nei_llog(cfg_id,
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
  nei_log_remove_config(cfg_id);
}

TEST(LogCTest, AsyncPipelineLengthModifiersLdJzLf) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st config = *nei_log_default_config();
  config.sinks[0] = &sink;
  config.sinks[1] = nullptr;
  const char cfg_id[] = "cfg-lenmod";
  ASSERT_EQ(nei_log_add_config(cfg_id, &config), 0);

  const long lv = 99;
  const intmax_t jv = 7;
  const size_t zv = 8;
  const long double ldv = static_cast<long double>(2.5);

  nei_llog(cfg_id, NEI_LOG_LEVEL_DEBUG, "fmt_test.c", 1, "fn", "ld=%ld jd=%jd zu=%zu Lf=%.3Lf", lv, jv, zv, ldv);

  nei_log_flush();
  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  ASSERT_FALSE(collector.messages.empty());
  const std::string &msg = collector.messages[0];
  EXPECT_NE(msg.find("ld=99"), std::string::npos);
  EXPECT_NE(msg.find("jd=7"), std::string::npos);
  EXPECT_NE(msg.find("zu=8"), std::string::npos);
  EXPECT_NE(msg.find("Lf=2.500"), std::string::npos);
  nei_log_remove_config(cfg_id);
}

TEST(LogCTest, DirectLlogWithDefaultConfigUsesSink) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  DefaultConfigSinkGuard guard;
  guard.set_primary_sink(&sink);

  nei_llog(NEI_LOG_DEFAULT_CONFIG_ID, NEI_LOG_LEVEL_DEBUG, __FILE__, __LINE__, "direct-fn", "x=%d", 1);

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
    nei_llog(NEI_LOG_DEFAULT_CONFIG_ID, NEI_LOG_LEVEL_INFO, __FILE__, __LINE__, "burst", "burst id=%d", i);
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

  const char id[] = "cfg-add-get-1";
  EXPECT_EQ(nei_log_add_config(id, &cfg), 0);

  const nei_log_config_st *got = nei_log_get_config(id);
  ASSERT_NE(got, nullptr);

  nei_llog(id, NEI_LOG_LEVEL_INFO, __FILE__, __LINE__, "config-test", "hello=%d", 42);

  nei_log_flush();

  std::lock_guard<std::mutex> lock(collector.mu);
  ASSERT_EQ(collector.messages.size(), 1U);
  EXPECT_NE(collector.messages[0].find("[I]"), std::string::npos);
  EXPECT_NE(collector.messages[0].find("hello=42"), std::string::npos);

  nei_log_remove_config(id);
}

TEST(LogCTest, ConfigRemoveByIdReturnsNull) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st cfg = *nei_log_default_config();
  cfg.sinks[0] = &sink;
  cfg.sinks[1] = nullptr;

  const char id[] = "cfg-remove-1";
  EXPECT_EQ(nei_log_add_config(id, &cfg), 0);

  EXPECT_NE(nei_log_get_config(id), nullptr);
  nei_log_remove_config(id);
  EXPECT_EQ(nei_log_get_config(id), nullptr);
}

TEST(LogCTest, ConfigCapacityMax16IncludingDefault) {
  LogCollector collector;
  nei_log_sink_st sink = {};
  sink.llog = CollectLevelLog;
  sink.opaque = &collector;

  nei_log_config_st cfg = *nei_log_default_config();
  cfg.sinks[0] = &sink;
  cfg.sinks[1] = nullptr;

  std::vector<std::string> ids;
  ids.reserve(16);

  // default config already occupies 1, so we can add up to 15 more.
  for (size_t i = 0; i < 15U; ++i) {
    std::string id = "cfg-cap-" + std::to_string(i);
    EXPECT_EQ(nei_log_add_config(id.c_str(), &cfg), 0);
    ids.emplace_back(id);
  }

  // Next add should fail.
  const char *too_many_id = "cfg-cap-too-many";
  EXPECT_LT(nei_log_add_config(too_many_id, &cfg), 0);

  // Cleanup so other tests won't observe a full table.
  for (const auto &id : ids) {
    nei_log_remove_config(id.c_str());
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
  const char cfg_id[] = "cfg-file-sink";
  ASSERT_EQ(nei_log_add_config(cfg_id, &cfg), 0);

  nei_llog(cfg_id, NEI_LOG_LEVEL_INFO, __FILE__, __LINE__, "file-sink", "info=%d", 11);
  nei_llog(cfg_id, NEI_LOG_LEVEL_DEBUG, __FILE__, __LINE__, "file-sink", "debug=%d", 22);
  nei_vlog(cfg_id, 1, __FILE__, __LINE__, "file-sink", "verbose=%d", 33);
  nei_vlog(cfg_id, 4, __FILE__, __LINE__, "file-sink", "verbose_drop=%d", 44);
  nei_log_flush();

  nei_log_remove_config(cfg_id);
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
  const char cfg_id[] = "cfg-race-safe";

  std::atomic<int> stop{0};
  std::thread producer([&]() {
    int i = 0;
    while (stop.load(std::memory_order_relaxed) == 0) {
      nei_llog(cfg_id, NEI_LOG_LEVEL_INFO, __FILE__, __LINE__, "race", "runtime=%d", i);
      ++i;
    }
  });

  std::thread manager([&]() {
    for (int i = 0; i < 200; ++i) {
      if ((i % 2) == 0) {
        (void)nei_log_add_config(cfg_id, &cfg);
      } else {
        nei_log_remove_config(cfg_id);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(1, std::memory_order_relaxed);
  });

  manager.join();
  producer.join();
  nei_log_flush();
  nei_log_remove_config(cfg_id);

  std::lock_guard<std::mutex> lock(collector.mu);
  EXPECT_GT(collector.messages.size(), 0U);
}
