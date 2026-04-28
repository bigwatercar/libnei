#include <stdio.h>

#include <nei/log/log.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

int main(void) {
#if defined(_WIN32)
  const char *log_path = "C:\\var\\nei_immediate_crash_demo.log";
  (void)CreateDirectoryA("C:\\var", NULL);
#else
  const char *log_path = "/tmp/nei_immediate_crash_demo.log";
#endif

  printf("NEI Immediate Crash Demo: writing logs then triggering fatal crash.\n");
  printf("Log file: %s\n", log_path);

  nei_log_default_file_sink_options_st sink_opts = nei_log_default_file_sink_options();
  sink_opts.flush_interval = 0U;
  sink_opts.write_batch_bytes = 0U;
  nei_log_sink_st *sink = nei_log_create_default_file_sink(log_path, &sink_opts);
  if (sink == NULL) {
    fprintf(stderr, "failed to create file sink\n");
    return 1;
  }

  nei_log_config_st *cfg = nei_log_default_config();
  cfg->log_to_console = 0;
  cfg->level_flags.all = 0xFFFFFFFFu;
  cfg->immediate_crash_on_fatal = 1U;
  cfg->sinks[0] = sink;
  cfg->sinks[1] = NULL;

  /* Install crash handler AFTER the sink is attached to the config so that
   * crash backtrace frames are routed to the log file as well as stderr. */
  if (nei_log_install_crash_handler(NEI_LOG_DEFAULT_CONFIG_HANDLE) != 0) {
    fprintf(stderr, "failed to install crash handler\n");
    return 1;
  }

  NEI_LOG_INFO("immediate crash demo started");
  NEI_LOG_WARN("next FATAL log will terminate the process immediately");
  nei_log_flush();
  NEI_LOG_FATAL("demo fatal: crash now (expected)");

  /* unreachable in normal flow */
  nei_log_flush();
  cfg->sinks[0] = NULL;
  nei_log_destroy_sink(sink);
  return 0;
}
