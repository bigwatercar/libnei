#pragma once

#ifndef NEI_TESTS_TEST_PLUGIN_NEI_TEST_PLUGIN_H_
#define NEI_TESTS_TEST_PLUGIN_NEI_TEST_PLUGIN_H_

// Export macro for the test plugin modules. The plugin targets are not linked
// against anything; they exist only to be loaded at runtime by
// native_library_test via NativeLibrary::Load().
#if defined(_WIN32)
#define NEI_TEST_PLUGIN_EXPORT __declspec(dllexport)
#else
#define NEI_TEST_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

NEI_TEST_PLUGIN_EXPORT int PluginAdd(int a, int b);
NEI_TEST_PLUGIN_EXPORT int PluginGetGlobal(void);
NEI_TEST_PLUGIN_EXPORT void PluginSetGlobal(int value);

// Exported data symbol: verifies that GetFunctionPointer resolves data
// symbols as well as code symbols.
extern NEI_TEST_PLUGIN_EXPORT int g_plugin_counter;

} // extern "C"

#endif // NEI_TESTS_TEST_PLUGIN_NEI_TEST_PLUGIN_H_
