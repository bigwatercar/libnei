// nei_global — library-internal diagnostic logging channel.
//
// g_nei_logger is the config handle all in-library NEI_LOG_C diagnostics are
// routed through.  It defaults to NEI_LOG_INVALID_CONFIG_HANDLE (silent) so
// the library never writes to the client's stdout/stderr unless explicitly
// enabled (Chromium-style: library diagnostics stay off unless requested).
#include <nei/build/nei_global.h>

#include <nei/log/log.h>

#include <string.h>

NEI_API uintptr_t g_nei_logger = NEI_LOG_INVALID_CONFIG_HANDLE;

void nei_enable_diagnostic_message_to_stdout(void) {
  if (g_nei_logger != NEI_LOG_INVALID_CONFIG_HANDLE)
    return;

  nei_log_config_st cfg = *nei_log_default_config();
  cfg.level_flags.all = 0xffffffffU;
  cfg.verbose_threshold = -1;
  // Library diagnostics go ONLY to stdout — never borrow the default
  // config's sinks (those belong to the client).
  memset(cfg.sinks, 0, sizeof(cfg.sinks));

  nei_log_sink_st *sink = nei_log_create_stdout_sink();
  if (sink == NULL)
    return;
  cfg.sinks[0] = sink;

  nei_log_config_handle_t handle = NEI_LOG_INVALID_CONFIG_HANDLE;
  if (nei_log_add_config(&cfg, &handle) == 0) {
    // Ownership of the sink transferred to the new config; it is released
    // by nei_log_remove_config in nei_shutdown_diagnostic_message.
    g_nei_logger = handle;
  } else {
    nei_log_release_sink(sink);
  }
}

void nei_shutdown_diagnostic_message(void) {
  if (g_nei_logger == NEI_LOG_INVALID_CONFIG_HANDLE)
    return;
  // Removes the config (double-flush + sink release) and invalidates the
  // handle.
  nei_log_remove_config(g_nei_logger);
  g_nei_logger = NEI_LOG_INVALID_CONFIG_HANDLE;
}
