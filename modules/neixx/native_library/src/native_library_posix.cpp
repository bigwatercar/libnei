#if !defined(_WIN32)

#include <dlfcn.h>

#include <neixx/native_library/native_library.h>

namespace nei {

NativeLibrary LoadNativeLibraryWithOptions(const std::filesystem::path &library_path,
                                           const NativeLibraryOptions &options,
                                           NativeLibraryLoadError *error) {
  int flags = 0;
  switch (options.symbol_resolution) {
  case NativeLibraryOptions::SymbolResolution::kLazy:
    flags |= RTLD_LAZY;
    break;
  case NativeLibraryOptions::SymbolResolution::kNow:
    flags |= RTLD_NOW;
    break;
  }
  switch (options.symbol_visibility) {
  case NativeLibraryOptions::SymbolVisibility::kLocal:
    flags |= RTLD_LOCAL;
    break;
  case NativeLibraryOptions::SymbolVisibility::kGlobal:
    flags |= RTLD_GLOBAL;
    break;
  }

  void *handle = ::dlopen(library_path.c_str(), flags);
  if (!handle) {
    if (error) {
      const char *dl_error = ::dlerror();
      error->message = dl_error ? dl_error : "dlopen failed";
    }
    return nullptr;
  }
  return handle;
}

void UnloadNativeLibrary(NativeLibrary library) {
  if (library)
    ::dlclose(library);
}

void *GetFunctionPointerFromNativeLibrary(NativeLibrary library, const char *name) {
  return ::dlsym(library, name);
}

} // namespace nei

#endif // !defined(_WIN32)
