#include <gtest/gtest.h>

#include <nei/build/nei_global.h>
#include <nei/log/log.h>

namespace {

// 库诊断通道的开启/关闭语义：幂等、独立 stdout 通道、可重复启用。
TEST(NeiGlobalTest, EnableShutdownLifecycle) {
  // 测试进程可能已通过其他路径启用过——先关闭，保证从无效句柄开始。
  nei_shutdown_diagnostic_message();
  EXPECT_EQ(g_nei_logger, NEI_LOG_INVALID_CONFIG_HANDLE);

  nei_enable_diagnostic_message_to_stdout();
  const nei_log_config_handle_t h1 = g_nei_logger;
  EXPECT_NE(h1, NEI_LOG_INVALID_CONFIG_HANDLE);

  // 幂等：重复 enable 不得更换句柄。
  nei_enable_diagnostic_message_to_stdout();
  EXPECT_EQ(h1, g_nei_logger);

  // 库诊断日志可经该通道写出（不崩溃、不丢 handle）。
  NEI_LOG_C(g_nei_logger, NEI_L_ERROR, "nei_global_test diagnostic");

  nei_shutdown_diagnostic_message();
  EXPECT_EQ(NEI_LOG_INVALID_CONFIG_HANDLE, g_nei_logger);

  // 关闭后可再次启用。
  nei_enable_diagnostic_message_to_stdout();
  EXPECT_NE(g_nei_logger, NEI_LOG_INVALID_CONFIG_HANDLE);
  nei_shutdown_diagnostic_message();
}

// 对无效句柄的 shutdown 必须 no-op（不允许影响他人配置）。
TEST(NeiGlobalTest, ShutdownWhenInvalidIsNoop) {
  nei_shutdown_diagnostic_message();
  EXPECT_EQ(g_nei_logger, NEI_LOG_INVALID_CONFIG_HANDLE);
  nei_shutdown_diagnostic_message();
  EXPECT_EQ(g_nei_logger, NEI_LOG_INVALID_CONFIG_HANDLE);
}

} // namespace
