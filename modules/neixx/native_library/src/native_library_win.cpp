#if defined(_WIN32)

#include <windows.h>

#include <neixx/native_library/native_library.h>
#include <neixx/strings/utf_string_conversions.h>

namespace nei {

NativeLibrary LoadNativeLibraryWithOptions(const std::filesystem::path &library_path,
                                           const NativeLibraryOptions &options,
                                           NativeLibraryLoadError *error) {
  // The options have no Windows equivalent. The search flags mirror Chromium:
  // look in the library's own directory first so that sibling dependencies
  // are found, then fall back to the default search order.
  HMODULE module = ::LoadLibraryExW(
      library_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!module) {
    if (error)
      error->code = static_cast<std::uint32_t>(::GetLastError());
    return nullptr;
  }
  return static_cast<NativeLibrary>(module);
}

NativeLibrary LoadSystemLibrary(std::string_view name, NativeLibraryLoadError *error) {
  std::u16string wide_name = UTF8ToUTF16(name);
  const wchar_t *wide_cstr = reinterpret_cast<const wchar_t *>(wide_name.c_str());

  HMODULE module = nullptr;
  if (::GetModuleHandleExW(0, wide_cstr, &module))
    return static_cast<NativeLibrary>(module);

  module = ::LoadLibraryExW(wide_cstr, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!module) {
    if (error)
      error->code = static_cast<std::uint32_t>(::GetLastError());
    return nullptr;
  }
  return static_cast<NativeLibrary>(module);
}

void UnloadNativeLibrary(NativeLibrary library) {
  if (library)
    ::FreeLibrary(static_cast<HMODULE>(library));
}

void *GetFunctionPointerFromNativeLibrary(NativeLibrary library, const char *name) {
  return reinterpret_cast<void *>(::GetProcAddress(static_cast<HMODULE>(library), name));
}

} // namespace nei

#endif // defined(_WIN32)
