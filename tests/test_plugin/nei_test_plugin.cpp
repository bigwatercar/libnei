#include "nei_test_plugin.h"

extern "C" {

NEI_TEST_PLUGIN_EXPORT int g_plugin_counter = 42;

NEI_TEST_PLUGIN_EXPORT int PluginAdd(int a, int b) {
  return a + b;
}

NEI_TEST_PLUGIN_EXPORT int PluginGetGlobal(void) {
  return g_plugin_counter;
}

NEI_TEST_PLUGIN_EXPORT void PluginSetGlobal(int value) {
  g_plugin_counter = value;
}

} // extern "C"
