#include <neixx/strings/string_util.h>
#include <neixx/strings/split_string.h>
#include <neixx/strings/string_number_conversions.h>
#include <neixx/strings/utf_string_conversions.h>

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

int main() {
  const std::string utf8 = "  Chromium style: \xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe7\xac\xa6\xe8\xbd\xac\xe6\x8d\xa2  ";

  const std::string utf8_trimmed = nei::TrimWhitespace(utf8, nei::TrimPositions::kAll);
  const bool starts_with = nei::StartsWith(utf8_trimmed, "chromium", nei::CompareCase::kInsensitiveASCII);
  const std::u16string utf16 = nei::UTF8ToUTF16(utf8);
  const std::u16string utf16_trimmed = nei::TrimWhitespace(std::u16string_view(utf16), nei::TrimPositions::kAll);
  const bool starts_with_u16 = nei::StartsWith(std::u16string_view(utf16_trimmed),
                                               nei::UTF8ToUTF16("chromium"),
                                               nei::CompareCase::kInsensitiveASCII);

  const std::string roundtrip = nei::UTF16ToUTF8(utf16);
  const std::u16string ascii16 = nei::ASCIIToUTF16("ASCII demo");
  const std::string formatted = nei::StringPrintf("trimmed_len=%zu, starts_with=%d, starts_with_u16=%d",
                                                  utf8_trimmed.size(),
                                                  starts_with ? 1 : 0,
                                                  starts_with_u16 ? 1 : 0);
  const std::string csv = " 42, , 7, -3 , word, 100 ";
  const std::vector<std::string> csv_parts =
      nei::SplitString(csv, ',', nei::TRIM_WHITESPACE, nei::SPLIT_WANT_ALL);

  std::cout << "UTF-8 input     : " << utf8 << "\n";
  std::cout << "UTF-8 trimmed   : " << utf8_trimmed << "\n";
  std::cout << "StartsWith(UTF8): " << (starts_with ? "true" : "false") << "\n";
  std::cout << "UTF-16 units    : " << utf16.size() << "\n";
  std::cout << "StartsWith(U16) : " << (starts_with_u16 ? "true" : "false") << "\n";
  std::cout << "Roundtrip UTF-8 : " << roundtrip << "\n";
  std::cout << "ASCII->UTF16 len: " << ascii16.size() << "\n";
  std::cout << "StringPrintf    : " << formatted << "\n";
  std::cout << "CSV split       :";
  for (const std::string &part : csv_parts) {
    std::cout << " [" << part << "]";
  }
  std::cout << "\n";

  std::cout << "CSV integers    :";
  for (const std::string &part : csv_parts) {
    std::int64_t parsed = 0;
    if (nei::StringToInt64(part, &parsed)) {
      std::cout << ' ' << parsed;
    }
  }
  std::cout << "\n";

  std::cout << "UTF-16 hex      : ";
  for (char16_t cu : utf16) {
    std::cout << "0x" << std::hex << std::uppercase << static_cast<unsigned int>(cu) << ' ';
  }
  std::cout << std::dec << "\n";

  return 0;
}