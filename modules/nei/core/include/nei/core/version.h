#ifndef NEI_CORE_VERSION_H_
#define NEI_CORE_VERSION_H_

#include <nei/build/nei_export.h>

#ifdef __cplusplus
extern "C" {
#endif

// Version information of the loaded libnei library.  The values are compiled
// into the library from the generated nei/build/version.h header and must
// match the package version advertised by the CMake package config.

typedef struct nei_version_info {
  unsigned int major;
  unsigned int minor;
  unsigned int patch;
} nei_version_info;

// Returns the library version string, e.g. "0.9.0".  Points to static
// storage; the pointer stays valid for the process lifetime and the function
// is safe to call from any thread.
NEI_API const char *nei_get_version_string(void);

// Returns the version as a plain POD struct (safe for C and C++ callers).
NEI_API nei_version_info nei_get_version_info(void);

#ifdef __cplusplus
}
#endif

#endif // NEI_CORE_VERSION_H_
