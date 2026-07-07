/**
 * @file path_service_win.cpp
 * @brief Windows platform implementation of DefaultProvider.
 */

#if defined(_WIN32)

#include "path_service_impl.h"

#include "nei/debug/check.h"
#include "neixx/strings/string_util.h"

#include <windows.h>
#include <KnownFolders.h>
#include <shlobj.h>

namespace nei {

// ---------------------------------------------------------------------------
// Helper: resolve a KNOWNFOLDERID to a std::filesystem::path.
// ---------------------------------------------------------------------------
static bool GetKnownFolder(const KNOWNFOLDERID &folderId,
                           std::filesystem::path *result) {
  PWSTR raw = nullptr;
  const HRESULT hr =
      SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &raw);
  if (FAILED(hr)) {
    DCHECK(false);
    return false;
  }
  DCHECK(raw != nullptr);
  *result = std::filesystem::path(raw);
  CoTaskMemFree(raw);
  return true;
}

// =============================================================================
// DefaultProvider  --  platform path resolution (Windows)
// =============================================================================

bool PathService::Impl::DefaultProvider(int key,
                                        std::filesystem::path *result) {
  DCHECK(result != nullptr);
  if (result == nullptr) {
    return false;
  }

  switch (static_cast<PathKeys>(key)) {

  // -----------------------------------------------------------------------
  // DIR_CURRENT
  // -----------------------------------------------------------------------
  case PathKeys::DIR_CURRENT: {
    std::error_code ec;
    *result = std::filesystem::current_path(ec);
    DCHECK(!ec);
    return !ec;
  }

  // -----------------------------------------------------------------------
  // DIR_EXE / FILE_EXE
  // -----------------------------------------------------------------------
  case PathKeys::DIR_EXE:
  case PathKeys::FILE_EXE: {
    WCHAR buf[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    DCHECK(len > 0 && len < MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
      return false;
    }
    const std::filesystem::path exe_path(buf);
    if (key == static_cast<int>(PathKeys::FILE_EXE)) {
      *result = exe_path;
    } else {
      *result = exe_path.parent_path();
    }
    return true;
  }

  // -----------------------------------------------------------------------
  // DIR_TEMP
  // -----------------------------------------------------------------------
  case PathKeys::DIR_TEMP: {
    WCHAR buf[MAX_PATH + 1];
    const DWORD len = GetTempPathW(MAX_PATH + 1, buf);
    DCHECK(len > 0);
    if (len == 0) {
      return false;
    }
    *result = std::filesystem::path(buf);
    return true;
  }

  // -----------------------------------------------------------------------
  // DIR_USER_DATA
  // -----------------------------------------------------------------------
  case PathKeys::DIR_USER_DATA: {
    return GetKnownFolder(FOLDERID_LocalAppData, result);
  }

  // -----------------------------------------------------------------------
  // DIR_PROGRAM_DATA
  // -----------------------------------------------------------------------
  case PathKeys::DIR_PROGRAM_DATA: {
    return GetKnownFolder(FOLDERID_ProgramData, result);
  }

  // -----------------------------------------------------------------------
  // DIR_USER_DESKTOP
  // -----------------------------------------------------------------------
  case PathKeys::DIR_USER_DESKTOP: {
    return GetKnownFolder(FOLDERID_Desktop, result);
  }

  // -----------------------------------------------------------------------
  // DIR_USER_DOCUMENTS
  // -----------------------------------------------------------------------
  case PathKeys::DIR_USER_DOCUMENTS: {
    return GetKnownFolder(FOLDERID_Documents, result);
  }

  // -----------------------------------------------------------------------
  // DIR_USER_MUSIC
  // -----------------------------------------------------------------------
  case PathKeys::DIR_USER_MUSIC: {
    return GetKnownFolder(FOLDERID_Music, result);
  }

  // -----------------------------------------------------------------------
  // DIR_USER_VIDEO
  // -----------------------------------------------------------------------
  case PathKeys::DIR_USER_VIDEO: {
    return GetKnownFolder(FOLDERID_Videos, result);
  }

  // -----------------------------------------------------------------------
  // DIR_USER_DOWNLOADS
  // -----------------------------------------------------------------------
  case PathKeys::DIR_USER_DOWNLOADS: {
    return GetKnownFolder(FOLDERID_Downloads, result);
  }

  // -----------------------------------------------------------------------
  // DIR_USER_PICTURES
  // -----------------------------------------------------------------------
  case PathKeys::DIR_USER_PICTURES: {
    return GetKnownFolder(FOLDERID_Pictures, result);
  }

  // -----------------------------------------------------------------------
  // Unknown key
  // -----------------------------------------------------------------------
  default: {
    DCHECK(false);
    return false;
  }
  }
}

}  // namespace nei

#endif  // _WIN32
