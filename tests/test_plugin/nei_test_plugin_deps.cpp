// POSIX-only companion plugin for the RTLD_GLOBAL/RTLD_LOCAL visibility test.
// It references PluginAdd() from nei_test_plugin, so it can only be dlopen'd
// while nei_test_plugin has been loaded with global symbol visibility.
#include "nei_test_plugin.h"

extern "C" {

NEI_TEST_PLUGIN_EXPORT int PluginCallAdd(int a, int b) {
  return PluginAdd(a, b);
}

} // extern "C"
