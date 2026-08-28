#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>

#include <neixx/native_library/native_library.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace nei {
namespace {

using PluginAddFn = int (*)(int, int);
using PluginCallAddFn = int (*)(int, int);

// Returns the directory containing the nei_tests executable. The test plugin
// modules are built into the same directory, so the plugin path is derived
// from it.
std::filesystem::path GetTestExeDir() {
#if defined(_WIN32)
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  EXPECT_GT(length, 0u);
  return std::filesystem::path(std::wstring(buffer, length)).parent_path();
#else
  char buffer[4096] = {};
  const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  EXPECT_GT(length, 0);
  buffer[length] = '\0';
  return std::filesystem::path(buffer).parent_path();
#endif
}

std::filesystem::path GetPluginPath(std::string_view name) {
  return GetTestExeDir() / GetNativeLibraryName(name);
}

TEST(NativeLibraryTest, LoadAndCallFunction) {
  NativeLibraryLoadError error;
  ScopedNativeLibrary lib = LoadNativeLibrary(GetPluginPath("nei_test_plugin"), &error);
  ASSERT_TRUE(lib.is_valid()) << error.ToString();

  void *symbol = GetFunctionPointerFromNativeLibrary(lib.get(), "PluginAdd");
  ASSERT_NE(symbol, nullptr);
  auto add = reinterpret_cast<PluginAddFn>(symbol);
  EXPECT_EQ(add(2, 3), 5);
  EXPECT_EQ(add(-7, 7), 0);
}

TEST(NativeLibraryTest, LoadAndReadDataSymbol) {
  NativeLibraryLoadError error;
  ScopedNativeLibrary lib = LoadNativeLibrary(GetPluginPath("nei_test_plugin"), &error);
  ASSERT_TRUE(lib.is_valid()) << error.ToString();

  int *counter = reinterpret_cast<int *>(GetFunctionPointerFromNativeLibrary(lib.get(), "g_plugin_counter"));
  ASSERT_NE(counter, nullptr);
  EXPECT_EQ(*counter, 42);
}

TEST(NativeLibraryTest, LoadMissingLibraryFails) {
  NativeLibraryLoadError error;
  ScopedNativeLibrary lib = LoadNativeLibrary(GetTestExeDir() / "no_such_library_at_all", &error);
  EXPECT_FALSE(lib.is_valid());
  EXPECT_FALSE(error.ok());
  EXPECT_FALSE(error.ToString().empty());
}

TEST(NativeLibraryTest, ResolveUnknownSymbolReturnsNull) {
  NativeLibraryLoadError error;
  ScopedNativeLibrary lib = LoadNativeLibrary(GetPluginPath("nei_test_plugin"), &error);
  ASSERT_TRUE(lib.is_valid()) << error.ToString();
  EXPECT_EQ(GetFunctionPointerFromNativeLibrary(lib.get(), "NoSuchSymbol"), nullptr);
}

TEST(NativeLibraryTest, MoveSemanticsPreserveHandle) {
  NativeLibraryLoadError error;
  ScopedNativeLibrary lib = LoadNativeLibrary(GetPluginPath("nei_test_plugin"), &error);
  ASSERT_TRUE(lib.is_valid()) << error.ToString();
  NativeLibrary raw_handle = lib.get();
  EXPECT_NE(raw_handle, nullptr);

  ScopedNativeLibrary moved(std::move(lib));
  EXPECT_FALSE(lib.is_valid());
  ASSERT_TRUE(moved.is_valid());
  EXPECT_EQ(moved.get(), raw_handle);

  ScopedNativeLibrary assigned;
  assigned = std::move(moved);
  EXPECT_FALSE(moved.is_valid());
  ASSERT_TRUE(assigned.is_valid());
  EXPECT_EQ(assigned.get(), raw_handle);
}

TEST(NativeLibraryTest, ResetUnloadsAndInvalidates) {
  NativeLibraryLoadError error;
  ScopedNativeLibrary lib = LoadNativeLibrary(GetPluginPath("nei_test_plugin"), &error);
  ASSERT_TRUE(lib.is_valid()) << error.ToString();

  lib.reset();
  EXPECT_FALSE(lib.is_valid());

  // Resetting an invalid handle is a no-op.
  lib.reset();
  EXPECT_FALSE(lib.is_valid());
}

TEST(NativeLibraryTest, RawHandleManualUnload) {
  // The raw handle can be held for the whole process lifetime (e.g. stored in
  // a top-level global) and unloaded manually whenever appropriate.
  NativeLibraryLoadError error;
  NativeLibrary raw = LoadNativeLibrary(GetPluginPath("nei_test_plugin"), &error);
  ASSERT_NE(raw, nullptr) << error.ToString();

  void *symbol = GetFunctionPointerFromNativeLibrary(raw, "PluginGetGlobal");
  ASSERT_NE(symbol, nullptr);
  auto get_global = reinterpret_cast<int (*)()>(symbol);
  EXPECT_EQ(get_global(), 42);

  UnloadNativeLibrary(raw);
}

TEST(NativeLibraryTest, ReleaseTransfersOwnership) {
  NativeLibraryLoadError error;
  ScopedNativeLibrary lib = LoadNativeLibrary(GetPluginPath("nei_test_plugin"), &error);
  ASSERT_TRUE(lib.is_valid()) << error.ToString();

  NativeLibrary raw = lib.release();
  EXPECT_FALSE(lib.is_valid());
  EXPECT_NE(raw, nullptr);
  EXPECT_NE(GetFunctionPointerFromNativeLibrary(raw, "PluginAdd"), nullptr);

  // Ownership moved to the caller; unloading is now the caller's job.
  UnloadNativeLibrary(raw);
}

TEST(NativeLibraryTest, PlatformNames) {
#if defined(_WIN32)
  EXPECT_EQ(GetNativeLibraryName("mylib"), "mylib.dll");
  EXPECT_EQ(GetLoadableModuleName("mylib"), "mylib.dll");
#elif defined(__APPLE__)
  EXPECT_EQ(GetNativeLibraryName("mylib"), "libmylib.dylib");
  EXPECT_EQ(GetLoadableModuleName("mylib"), "mylib.so");
#else
  EXPECT_EQ(GetNativeLibraryName("mylib"), "libmylib.so");
  EXPECT_EQ(GetLoadableModuleName("mylib"), "libmylib.so");
#endif
}

TEST(NativeLibraryTest, LoadWithOptionsNow) {
  NativeLibraryOptions options;
  options.symbol_resolution = NativeLibraryOptions::SymbolResolution::kNow;

  NativeLibraryLoadError error;
  ScopedNativeLibrary lib = LoadNativeLibraryWithOptions(GetPluginPath("nei_test_plugin"), options, &error);
  ASSERT_TRUE(lib.is_valid()) << error.ToString();

  void *symbol = GetFunctionPointerFromNativeLibrary(lib.get(), "PluginGetGlobal");
  ASSERT_NE(symbol, nullptr);
  auto get_global = reinterpret_cast<int (*)()>(symbol);
  EXPECT_EQ(get_global(), 42);
}

TEST(NativeLibraryTest, GlobalSymbolVisibility) {
#if defined(_WIN32)
  GTEST_SKIP() << "RTLD_GLOBAL/RTLD_LOCAL semantics are POSIX-only";
#else
  NativeLibraryOptions local_options; // Defaults: kLazy, kLocal.

  NativeLibraryLoadError error;
  ScopedNativeLibrary local_base =
      LoadNativeLibraryWithOptions(GetPluginPath("nei_test_plugin"), local_options, &error);
  ASSERT_TRUE(local_base.is_valid()) << error.ToString();

  // The dependency plugin must be loaded with kNow resolution: RTLD_LAZY
  // defers binding of its PluginAdd reference to first use, so dlopen would
  // succeed even when the symbol is not visible.
  NativeLibraryOptions deps_now;
  deps_now.symbol_resolution = NativeLibraryOptions::SymbolResolution::kNow;

  // With kLocal visibility the dependency plugin cannot resolve PluginAdd.
  NativeLibraryLoadError deps_error;
  ScopedNativeLibrary deps_hidden =
      LoadNativeLibraryWithOptions(GetPluginPath("nei_test_plugin_deps"), deps_now, &deps_error);
  EXPECT_FALSE(deps_hidden.is_valid());
  EXPECT_FALSE(deps_error.ok());

  // Unloading the first plugin drops its symbols from the global scope.
  local_base.reset();

  NativeLibraryOptions global_options;
  global_options.symbol_visibility = NativeLibraryOptions::SymbolVisibility::kGlobal;

  NativeLibraryLoadError base_error;
  ScopedNativeLibrary global_base =
      LoadNativeLibraryWithOptions(GetPluginPath("nei_test_plugin"), global_options, &base_error);
  ASSERT_TRUE(global_base.is_valid()) << base_error.ToString();

  // With kGlobal visibility the dependency plugin resolves PluginAdd.
  NativeLibraryLoadError deps_ok_error;
  ScopedNativeLibrary deps_visible =
      LoadNativeLibraryWithOptions(GetPluginPath("nei_test_plugin_deps"), deps_now, &deps_ok_error);
  ASSERT_TRUE(deps_visible.is_valid()) << deps_ok_error.ToString();

  void *symbol = GetFunctionPointerFromNativeLibrary(deps_visible.get(), "PluginCallAdd");
  ASSERT_NE(symbol, nullptr);
  auto call_add = reinterpret_cast<PluginCallAddFn>(symbol);
  EXPECT_EQ(call_add(20, 22), 42);
#endif
}

TEST(NativeLibraryTest, LoadSystemLibraryKernel32) {
#if defined(_WIN32)
  NativeLibraryLoadError error;
  ScopedNativeLibrary kernel32 = LoadSystemLibrary("kernel32.dll", &error);
  ASSERT_TRUE(kernel32.is_valid()) << error.ToString();
  EXPECT_NE(GetFunctionPointerFromNativeLibrary(kernel32.get(), "GetTickCount"), nullptr);
#else
  GTEST_SKIP() << "LoadSystemLibrary is Windows-only";
#endif
}

} // namespace
} // namespace nei
