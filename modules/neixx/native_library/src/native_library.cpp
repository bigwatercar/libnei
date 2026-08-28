#include <neixx/native_library/native_library.h>

namespace nei {

// ---------------------------------------------------------------------------
// NativeLibraryLoadError
// ---------------------------------------------------------------------------

std::string NativeLibraryLoadError::ToString() const {
  if (code != 0)
    return std::to_string(code);
  return message;
}

// ---------------------------------------------------------------------------
// Load / naming helpers shared across platforms
// ---------------------------------------------------------------------------

NativeLibrary LoadNativeLibrary(const std::filesystem::path &library_path, NativeLibraryLoadError *error) {
  return LoadNativeLibraryWithOptions(library_path, NativeLibraryOptions{}, error);
}

std::string GetNativeLibraryName(std::string_view name) {
#if defined(_WIN32)
  return std::string(name) + ".dll";
#elif defined(__APPLE__)
  return "lib" + std::string(name) + ".dylib";
#else
  return "lib" + std::string(name) + ".so";
#endif
}

std::string GetLoadableModuleName(std::string_view name) {
#if defined(__APPLE__)
  return std::string(name) + ".so";
#else
  return GetNativeLibraryName(name);
#endif
}

} // namespace nei
