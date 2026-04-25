#include <stdio.h>
#include <nei/log/log.h>

int main() {
  printf("NEI Log Demo - Demonstrating logging functionality\n");
  nei_log_sink_st *sink = nei_log_create_default_file_sink("test.log", NULL);
  if (sink == NULL) {
    return 1;
  }

  nei_log_config_st *cfg = nei_log_default_config();
  cfg->log_to_console = 0;
  cfg->level_flags.all = 0xffffffffU;
  cfg->verbose_threshold = 2;
  cfg->sinks[0] = sink;
  cfg->sinks[1] = NULL;

  NEI_LOG_INFO("Smoke test: sink ok");
  NEI_LOG_DEBUG("abc %s", "abc");
  NEI_LOG_INFO("XAXXX");

  nei_log_flush();
  cfg->sinks[0] = NULL;
  nei_log_destroy_sink(sink);
  return 0;
}
