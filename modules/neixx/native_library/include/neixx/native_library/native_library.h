#pragma once

#ifndef NEIXX_NATIVE_LIBRARY_NATIVE_LIBRARY_H_
#define NEIXX_NATIVE_LIBRARY_NATIVE_LIBRARY_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <nei/build/nei_export.h>

namespace nei {

// Opaque handle to a dynamically loaded library (HMODULE on Windows, the
// dlopen() handle on POSIX). Stored as void* so that no platform header leaks
// into this file; the load/unload functions cast it to the concrete platform
// type internally.
//
// Raw handles are not released automatically: every successful load must be
// matched with UnloadNativeLibrary(). Code that wants RAII should wrap the
// handle in ScopedNativeLibrary, while libraries that must stay loaded for
// the rest of the process lifetime (e.g. held in a top-level global) can keep
// the raw handle and intentionally skip the unload.
using NativeLibrary = void *;

// ---------------------------------------------------------------------------
// NativeLibraryLoadError -- error information for a failed library load
// ---------------------------------------------------------------------------
//
// Layered error model (mirrors AsyncFile::Error): a semantic status plus the
// platform-native diagnostic. On Windows |code| carries the Win32 error
// returned by GetLastError(); on POSIX |message| carries the text returned by
// dlerror().
struct NEI_API NativeLibraryLoadError {
  std::uint32_t code = 0; // Windows: GetLastError() value.
  std::string message;    // POSIX: dlerror() text.

  // True when no error information is recorded.
  bool ok() const {
    return code == 0 && message.empty();
  }

  // Returns a human-readable representation of the load error.
  std::string ToString() const;
};

// ---------------------------------------------------------------------------
// NativeLibraryOptions -- options controlling how a library is loaded
// ---------------------------------------------------------------------------
//
// The fields map to the dlopen() mode flags on POSIX. Windows has no direct
// equivalent, so both fields are ignored there.
struct NativeLibraryOptions {
  enum class SymbolResolution {
    kLazy, // RTLD_LAZY: resolve symbols when first referenced (default).
    kNow,  // RTLD_NOW:  resolve all symbols at load time.
  };
  enum class SymbolVisibility {
    kLocal, // RTLD_LOCAL: symbols stay private to this library (default).
    kGlobal // RTLD_GLOBAL: symbols are visible to subsequently loaded
            // libraries.
  };

  SymbolResolution symbol_resolution = SymbolResolution::kLazy;
  SymbolVisibility symbol_visibility = SymbolVisibility::kLocal;
};

// Loads a native library from disk. Returns nullptr on failure; fills
// |error| when it is non-null (|error| is left untouched on success). Release
// the handle with UnloadNativeLibrary() when done, or wrap it in
// ScopedNativeLibrary for automatic unloading.
NEI_API NativeLibrary LoadNativeLibrary(const std::filesystem::path &library_path,
                                        NativeLibraryLoadError *error = nullptr);

// Loads a native library with the given |options|.
NEI_API NativeLibrary LoadNativeLibraryWithOptions(const std::filesystem::path &library_path,
                                                   const NativeLibraryOptions &options,
                                                   NativeLibraryLoadError *error = nullptr);

#if defined(_WIN32)
// Loads a library from the Windows system directory. If the library is
// already loaded, a new reference to the existing module is returned.
NEI_API NativeLibrary LoadSystemLibrary(std::string_view name, NativeLibraryLoadError *error = nullptr);
#endif // defined(_WIN32)

// Unloads a native library. The library must not be used afterwards.
NEI_API void UnloadNativeLibrary(NativeLibrary library);

// Resolves the address of an exported symbol. Returns nullptr when the
// symbol is not found. The returned pointer stays valid until the library
// is unloaded. Only symbols with C linkage can be resolved on either
// platform.
NEI_API void *GetFunctionPointerFromNativeLibrary(NativeLibrary library, const char *name);

// Returns the platform-specific name for a native library: "mylib" becomes
// "mylib.dll" on Windows, "libmylib.so" on Linux and "libmylib.dylib" on
// macOS.
NEI_API std::string GetNativeLibraryName(std::string_view name);

// Returns the platform-specific name for a loadable module. Identical to
// GetNativeLibraryName() everywhere except macOS, where "mylib" becomes
// "mylib.so".
NEI_API std::string GetLoadableModuleName(std::string_view name);

// ---------------------------------------------------------------------------
// ScopedNativeLibrary -- RAII owner of a native library handle
// ---------------------------------------------------------------------------
//
// Wraps a NativeLibrary and unloads it on destruction. Move-only. This class
// is header-only and intentionally small so it stays cheap to use everywhere.
class ScopedNativeLibrary {
public:
  ScopedNativeLibrary() = default;

  // Takes ownership of |library|. Passing nullptr yields an invalid object.
  ScopedNativeLibrary(NativeLibrary library)
      : library_(library) {
  }

  ScopedNativeLibrary(const ScopedNativeLibrary &) = delete;
  ScopedNativeLibrary &operator=(const ScopedNativeLibrary &) = delete;

  ScopedNativeLibrary(ScopedNativeLibrary &&other) noexcept
      : library_(other.release()) {
  }

  ScopedNativeLibrary &operator=(ScopedNativeLibrary &&other) noexcept {
    reset(other.release());
    return *this;
  }

  ~ScopedNativeLibrary() {
    reset();
  }

  // True if a non-null library handle is owned.
  bool is_valid() const {
    return library_ != nullptr;
  }

  // Returns the owned handle without releasing it.
  NativeLibrary get() const {
    return library_;
  }

  // Implicit conversion to the raw handle.
  operator NativeLibrary() const {
    return get();
  }

  // Releases ownership without unloading. The caller becomes responsible
  // for calling UnloadNativeLibrary().
  NativeLibrary release() {
    NativeLibrary library = library_;
    library_ = nullptr;
    return library;
  }

  // Unloads the currently owned library (if any) and takes ownership of
  // |library|. reset() with no argument just unloads.
  void reset(NativeLibrary library = nullptr) {
    if (library_)
      UnloadNativeLibrary(library_);
    library_ = library;
  }

  void swap(ScopedNativeLibrary &other) {
    std::swap(library_, other.library_);
  }

private:
  NativeLibrary library_ = nullptr;
};

} // namespace nei

#endif // NEIXX_NATIVE_LIBRARY_NATIVE_LIBRARY_H_
