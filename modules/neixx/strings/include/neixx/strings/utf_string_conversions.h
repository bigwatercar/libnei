#ifndef NEIXX_STRINGS_UTF_STRING_CONVERSIONS_H_
#define NEIXX_STRINGS_UTF_STRING_CONVERSIONS_H_

#include <string>
#include <string_view>

#include <nei/macros/nei_export.h>

namespace nei {

// Converts between UTF-8 and UTF-16 using cross-platform, replacement-on-error
// semantics.  Uses std::string_view / std::string for UTF-8 regardless of
// C++ standard version, for maximum compatibility across C++17 and C++20.

NEI_API std::u16string UTF8ToUTF16(std::string_view utf8);
NEI_API std::string UTF16ToUTF8(std::u16string_view utf16);
NEI_API std::u16string ASCIIToUTF16(std::string_view ascii);

// =============================================================================
// System default codepage (ACP)  ↔  UTF-8 / UTF-16
// =============================================================================
//
// On Windows these functions use CP_ACP (the system ANSI code page — e.g.
// CP936 for Chinese Simplified, CP950 for Chinese Traditional).  On POSIX
// the system encoding is assumed to be UTF-8 (the default on all modern
// Linux and macOS distributions).
//
// Invalid byte sequences are replaced silently (no exceptions, no error
// codes).  This matches the behaviour of the C-layer nei_mbcs_to_utf8 /
// nei_utf8_to_mbcs and is appropriate for command-line arguments,
// environment variables, and legacy file paths.

// Convert a multi-byte string in the system default codepage to UTF-8.
//   "C:\中文路径" (CP936)  →  "C:\中文路径" (UTF-8)
NEI_API std::string SystemCodepageToUTF8(std::string_view mbcs);

// Convert a multi-byte string in the system default codepage to UTF-16.
NEI_API std::u16string SystemCodepageToUTF16(std::string_view mbcs);

// Convert a UTF-8 string to the system default codepage.
NEI_API std::string UTF8ToSystemCodepage(std::string_view utf8);

// Convert a UTF-16 string to the system default codepage.
NEI_API std::string UTF16ToSystemCodepage(std::u16string_view utf16);

} // namespace nei

#endif // NEIXX_STRINGS_UTF_STRING_CONVERSIONS_H_
