#include <neixx/strings/text_normalization.h>

#include <cstdint>

namespace nei {
namespace {

// ---- UTF-8 low-level helpers ------------------------------------------------

static void EncodeUTF8(char32_t cp, std::string &out) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else {
    // Replacement character U+FFFD
    out.push_back(static_cast<char>(0xEFu));
    out.push_back(static_cast<char>(0xBFu));
    out.push_back(static_cast<char>(0xBDu));
  }
}

// Validates one UTF-8 sequence starting at p[i] (len = total buffer length).
// On success, sets seq_len to the sequence length and returns true.
// On failure, seq_len is set to 1 so the caller can skip one byte.
static bool ValidateUTF8Sequence(const unsigned char *p,
                                 std::size_t len,
                                 std::size_t i,
                                 int &seq_len) {
  unsigned char c = p[i];
  if (c < 0x80) {
    seq_len = 1;
    return true;
  }
  if (c < 0xC2) { seq_len = 1; return false; } // 0x80-0xBF: lone continuation; 0xC0-0xC1: overlong
  if (c < 0xE0) { seq_len = 2; }
  else if (c < 0xF0) { seq_len = 3; }
  else if (c <= 0xF4) { seq_len = 4; }
  else { seq_len = 1; return false; }

  if (i + static_cast<std::size_t>(seq_len) > len) { seq_len = 1; return false; }

  for (int j = 1; j < seq_len; ++j) {
    if ((p[i + j] & 0xC0u) != 0x80u) { seq_len = 1; return false; }
  }

  // Overlong / surrogate / out-of-range checks
  if (seq_len == 3) {
    if (c == 0xE0 && p[i + 1] < 0xA0) { seq_len = 1; return false; } // overlong
    if (c == 0xED && p[i + 1] >= 0xA0) { seq_len = 1; return false; } // surrogate D800-DFFF
  } else if (seq_len == 4) {
    if (c == 0xF0 && p[i + 1] < 0x90) { seq_len = 1; return false; } // overlong
    if (c == 0xF4 && p[i + 1] > 0x8F) { seq_len = 1; return false; } // > U+10FFFF
  }

  return true;
}

// Decodes a validated UTF-8 sequence (seq_len must be correct) starting at p[i].
static char32_t DecodeUTF8Seq(const unsigned char *p, std::size_t i, int seq_len) {
  if (seq_len == 1) return p[i];
  if (seq_len == 2) return ((p[i] & 0x1Fu) << 6) | (p[i + 1] & 0x3Fu);
  if (seq_len == 3) {
    return ((p[i] & 0x0Fu) << 12) | ((p[i + 1] & 0x3Fu) << 6) | (p[i + 2] & 0x3Fu);
  }
  return ((p[i] & 0x07u) << 18) | ((p[i + 1] & 0x3Fu) << 12) |
         ((p[i + 2] & 0x3Fu) << 6) | (p[i + 3] & 0x3Fu);
}

// Iterates UTF-8 codepoints in |input|, applies |fn| to each, and writes the
// resulting codepoints back as UTF-8.  Invalid byte sequences emit U+FFFD.
template <typename Fn>
static std::string TransformUTF8(std::string_view input, Fn &&fn) {
  const auto *p = reinterpret_cast<const unsigned char *>(input.data());
  const std::size_t len = input.size();
  std::string out;
  out.reserve(len);

  std::size_t i = 0;
  while (i < len) {
    int seq_len = 1;
    bool valid = ValidateUTF8Sequence(p, len, i, seq_len);
    if (!valid) {
      EncodeUTF8(0xFFFDu, out);
      i += 1;
      continue;
    }
    char32_t cp = DecodeUTF8Seq(p, i, seq_len);
    EncodeUTF8(fn(cp), out);
    i += static_cast<std::size_t>(seq_len);
  }
  return out;
}

// ---- UTF-16 low-level helpers -----------------------------------------------

static void EncodeUTF16(char32_t cp, std::u16string &out) {
  if (cp < 0xD800 || (cp > 0xDFFF && cp <= 0xFFFF)) {
    out.push_back(static_cast<char16_t>(cp));
  } else if (cp <= 0x10FFFF) {
    cp -= 0x10000u;
    out.push_back(static_cast<char16_t>(0xD800u | (cp >> 10)));
    out.push_back(static_cast<char16_t>(0xDC00u | (cp & 0x3FFu)));
  } else {
    out.push_back(u'\uFFFD');
  }
}

// Iterates UTF-16 codepoints (handling surrogate pairs), applies |fn| to each.
// Lone surrogates emit U+FFFD.
template <typename Fn>
static std::u16string TransformUTF16(std::u16string_view input, Fn &&fn) {
  std::u16string out;
  out.reserve(input.size());

  std::size_t i = 0;
  while (i < input.size()) {
    char16_t c = input[i];
    char32_t cp;
    int advance = 1;

    if (c >= 0xD800u && c <= 0xDBFFu) {
      if (i + 1 < input.size()) {
        char16_t c2 = input[i + 1];
        if (c2 >= 0xDC00u && c2 <= 0xDFFFu) {
          cp = 0x10000u + (static_cast<char32_t>(c - 0xD800u) << 10) + (c2 - 0xDC00u);
          advance = 2;
        } else {
          cp = 0xFFFDu; // lone high surrogate
        }
      } else {
        cp = 0xFFFDu;
      }
    } else if (c >= 0xDC00u && c <= 0xDFFFu) {
      cp = 0xFFFDu; // lone low surrogate
    } else {
      cp = c;
    }

    EncodeUTF16(fn(cp), out);
    i += static_cast<std::size_t>(advance);
  }
  return out;
}

// ---- Codepoint mapping functions --------------------------------------------

// Fullwidth ASCII/Latin (U+FF01-U+FF5E) <-> ASCII (U+0021-U+007E)
// Ideographic space (U+3000) <-> regular space (U+0020)
static char32_t ToHalfWidthCP(char32_t cp) {
  if (cp >= 0xFF01u && cp <= 0xFF5Eu) return cp - 0xFF01u + 0x0021u;
  if (cp == 0x3000u) return 0x0020u;
  return cp;
}

static char32_t ToFullWidthCP(char32_t cp) {
  if (cp >= 0x0021u && cp <= 0x007Eu) return cp - 0x0021u + 0xFF01u;
  if (cp == 0x0020u) return 0x3000u;
  return cp;
}

// Common Chinese punctuation to ASCII (kZhToAscii mode).
static char32_t ChinesePunctToASCII(char32_t cp) {
  switch (cp) {
    case 0x3002u: return '.';
    case 0xFF0Cu: return ',';
    case 0x3001u: return ',';
    case 0xFF01u: return '!';
    case 0xFF1Fu: return '?';
    case 0xFF1Au: return ':';
    case 0xFF1Bu: return ';';
    case 0x300Cu: return '"';
    case 0x300Du: return '"';
    case 0x300Eu: return '"';
    case 0x300Fu: return '"';
    case 0x2018u: return '\'';
    case 0x2019u: return '\'';
    case 0x201Cu: return '"';
    case 0x201Du: return '"';
    case 0x2014u: return '-';
    case 0x2013u: return '-';
    case 0x2026u: return '.';
    case 0x00B7u: return '.';
    default:      return cp;
  }
}

// Collapses runs of ASCII spaces in an already-processed UTF-8 string.
static std::string CollapseSpaceRunsUTF8(std::string result) {
  std::string out;
  out.reserve(result.size());
  bool prev_space = false;
  for (char c : result) {
    if (c == ' ') {
      if (!prev_space) out.push_back(c);
      prev_space = true;
    } else {
      out.push_back(c);
      prev_space = false;
    }
  }
  return out;
}

static std::u16string CollapseSpaceRunsUTF16(std::u16string result) {
  std::u16string out;
  out.reserve(result.size());
  bool prev_space = false;
  for (char16_t c : result) {
    if (c == u' ') {
      if (!prev_space) out.push_back(c);
      prev_space = true;
    } else {
      out.push_back(c);
      prev_space = false;
    }
  }
  return out;
}

} // namespace

// ---- Public API -------------------------------------------------------------

bool NormalizeUnicode(std::string_view /*input*/,
                      UnicodeNormalizationForm /*form*/,
                      std::string * /*output*/) {
  // Full Unicode normalization (NFC/NFKC) requires an external library such as
  // ICU.  This build does not include one.
  return false;
}

bool NormalizeUnicode(std::u16string_view /*input*/,
                      UnicodeNormalizationForm /*form*/,
                      std::u16string * /*output*/) {
  return false;
}

std::string ToHalfWidth(std::string_view input) {
  return TransformUTF8(input, ToHalfWidthCP);
}

std::u16string ToHalfWidth(std::u16string_view input) {
  return TransformUTF16(input, ToHalfWidthCP);
}

std::string ToFullWidth(std::string_view input) {
  return TransformUTF8(input, ToFullWidthCP);
}

std::u16string ToFullWidth(std::u16string_view input) {
  return TransformUTF16(input, ToFullWidthCP);
}

std::string NormalizeChineseText(std::string_view input,
                                 SpaceNormalization space_mode,
                                 PunctuationNormalization punctuation_mode) {
  std::string result = TransformUTF8(input, [&](char32_t cp) -> char32_t {
    if (cp == 0x3000u) return 0x0020u; // ideographic space to space
    if (punctuation_mode == PunctuationNormalization::kZhToAscii)
      return ChinesePunctToASCII(cp);
    return cp;
  });
  if (space_mode == SpaceNormalization::kCollapseRuns)
    result = CollapseSpaceRunsUTF8(std::move(result));
  return result;
}

std::u16string NormalizeChineseText(std::u16string_view input,
                                    SpaceNormalization space_mode,
                                    PunctuationNormalization punctuation_mode) {
  std::u16string result = TransformUTF16(input, [&](char32_t cp) -> char32_t {
    if (cp == 0x3000u) return 0x0020u;
    if (punctuation_mode == PunctuationNormalization::kZhToAscii)
      return ChinesePunctToASCII(cp);
    return cp;
  });
  if (space_mode == SpaceNormalization::kCollapseRuns)
    result = CollapseSpaceRunsUTF16(std::move(result));
  return result;
}

bool IsValidUTF8(std::string_view input) {
  const auto *p = reinterpret_cast<const unsigned char *>(input.data());
  const std::size_t len = input.size();
  std::size_t i = 0;
  while (i < len) {
    int seq_len = 1;
    if (!ValidateUTF8Sequence(p, len, i, seq_len))
      return false;
    i += static_cast<std::size_t>(seq_len);
  }
  return true;
}

std::string FixInvalidUTF8(std::string_view input) {
  const auto *p = reinterpret_cast<const unsigned char *>(input.data());
  const std::size_t len = input.size();
  std::string out;
  out.reserve(len);

  static const char kReplacementUTF8[] = "\xEF\xBF\xBD"; // U+FFFD

  std::size_t i = 0;
  while (i < len) {
    int seq_len = 1;
    bool valid = ValidateUTF8Sequence(p, len, i, seq_len);
    if (valid) {
      out.append(input.data() + i, static_cast<std::size_t>(seq_len));
      i += static_cast<std::size_t>(seq_len);
    } else {
      out.append(kReplacementUTF8);
      i += 1;
    }
  }
  return out;
}

} // namespace nei
