#include <nei/core/version.h>

#include <nei/build/version.h>

const char *nei_get_version_string(void) {
  return NEI_VERSION_STRING;
}

nei_version_info nei_get_version_info(void) {
  nei_version_info info;
  info.major = NEI_VERSION_MAJOR;
  info.minor = NEI_VERSION_MINOR;
  info.patch = NEI_VERSION_PATCH;
  return info;
}
