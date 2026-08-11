#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <nei/utils/md5.h>
#include <nei/utils/sha1.h>
#include <nei/utils/uuid.h>

namespace {

std::string ToHex(const uint8_t *data, size_t len) {
  static const char hex[] = "0123456789abcdef";
  std::string out;
  out.resize(len * 2U);
  for (size_t i = 0; i < len; ++i) {
    out[i * 2U] = hex[(data[i] >> 4U) & 0x0FU];
    out[i * 2U + 1U] = hex[data[i] & 0x0FU];
  }
  return out;
}

bool IsLowerHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

std::filesystem::path MakeTempFilePath(const char *name_hint) {
  const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::filesystem::path p = std::filesystem::temp_directory_path();
  p /= std::string("nei_") + name_hint + "_" + std::to_string(static_cast<long long>(ticks)) + ".txt";
  return p;
}

void WriteFileAll(const std::filesystem::path &path, const std::string &content) {
  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
}

} // namespace

TEST(UtilsMd5Test, MatchesKnownVectors) {
  struct Case {
    const char *input;
    const char *hex;
  };

  const Case cases[] = {
      {"", "d41d8cd98f00b204e9800998ecf8427e"},
      {"abc", "900150983cd24fb0d6963f7d28e17f72"},
      {"The quick brown fox jumps over the lazy dog", "9e107d9d372bb6826bd81d3542a419d6"},
  };

  for (const Case &c : cases) {
    std::array<uint8_t, NEI_MD5_DIGEST_SIZE> digest{};
    nei_md5_sum(c.input, std::strlen(c.input), digest.data());
    EXPECT_EQ(ToHex(digest.data(), digest.size()), c.hex);
  }
}

TEST(UtilsMd5Test, HexAndFileHelpersWork) {
  const std::string input = "abc";
  std::array<uint8_t, NEI_MD5_DIGEST_SIZE> digest{};
  std::array<char, NEI_MD5_HEX_SIZE> hex{};
  const auto path = MakeTempFilePath("md5");

  nei_md5_sum(input.data(), input.size(), digest.data());
  nei_md5_to_hex(digest.data(), hex.data());
  EXPECT_STREQ(hex.data(), "900150983cd24fb0d6963f7d28e17f72");

  std::fill(hex.begin(), hex.end(), '\0');
  EXPECT_EQ(nei_md5_sum_hex(input.data(), input.size(), hex.data()), 0);
  EXPECT_STREQ(hex.data(), "900150983cd24fb0d6963f7d28e17f72");

  WriteFileAll(path, input);
  EXPECT_EQ(nei_md5_file_sum_hex(path.string().c_str(), hex.data()), 0);
  EXPECT_STREQ(hex.data(), "900150983cd24fb0d6963f7d28e17f72");
  std::filesystem::remove(path);
}

TEST(UtilsSha1Test, MatchesKnownVectors) {
  struct Case {
    const char *input;
    const char *hex;
  };

  const Case cases[] = {
      {"", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
      {"abc", "a9993e364706816aba3e25717850c26c9cd0d89d"},
      {"The quick brown fox jumps over the lazy dog", "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12"},
  };

  for (const Case &c : cases) {
    std::array<uint8_t, NEI_SHA1_DIGEST_SIZE> digest{};
    nei_sha1_sum(c.input, std::strlen(c.input), digest.data());
    EXPECT_EQ(ToHex(digest.data(), digest.size()), c.hex);
  }
}

TEST(UtilsSha1Test, HexAndFileHelpersWork) {
  const std::string input = "abc";
  std::array<uint8_t, NEI_SHA1_DIGEST_SIZE> digest{};
  std::array<char, NEI_SHA1_HEX_SIZE> hex{};
  const auto path = MakeTempFilePath("sha1");

  nei_sha1_sum(input.data(), input.size(), digest.data());
  nei_sha1_to_hex(digest.data(), hex.data());
  EXPECT_STREQ(hex.data(), "a9993e364706816aba3e25717850c26c9cd0d89d");

  std::fill(hex.begin(), hex.end(), '\0');
  EXPECT_EQ(nei_sha1_sum_hex(input.data(), input.size(), hex.data()), 0);
  EXPECT_STREQ(hex.data(), "a9993e364706816aba3e25717850c26c9cd0d89d");

  WriteFileAll(path, input);
  EXPECT_EQ(nei_sha1_file_sum_hex(path.string().c_str(), hex.data()), 0);
  EXPECT_STREQ(hex.data(), "a9993e364706816aba3e25717850c26c9cd0d89d");
  std::filesystem::remove(path);
}

TEST(UtilsUuidTest, GeneratesRfc4122V4String) {
  std::set<std::string> uuids;
  int saw_strong = 0;
  int saw_degraded = 0;

  for (int i = 0; i < 1000; ++i) {
    std::array<char, NEI_UUID_STRING_SIZE> s{};
    const int rc = nei_uuid4_generate_string(s.data());
    ASSERT_TRUE(rc == NEI_UUID_OK_STRONG || rc == NEI_UUID_OK_DEGRADED);
    if (rc == NEI_UUID_OK_STRONG) {
      saw_strong = 1;
    }
    if (rc == NEI_UUID_OK_DEGRADED) {
      saw_degraded = 1;
    }

    std::string value(s.data());
    ASSERT_EQ(value.size(), 36U);
    ASSERT_EQ(value[8], '-');
    ASSERT_EQ(value[13], '-');
    ASSERT_EQ(value[18], '-');
    ASSERT_EQ(value[23], '-');

    for (size_t idx = 0; idx < value.size(); ++idx) {
      if (idx == 8U || idx == 13U || idx == 18U || idx == 23U) {
        continue;
      }
      ASSERT_TRUE(IsLowerHex(value[idx])) << value;
    }

    ASSERT_EQ(value[14], '4') << value;
    ASSERT_TRUE(value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b') << value;

    uuids.insert(value);
  }

  EXPECT_EQ(uuids.size(), 1000U);
  EXPECT_TRUE(saw_strong != 0 || saw_degraded != 0);
}

TEST(UtilsUuidTest, FromStringParsesCanonicalForms) {
  struct Case {
    const char *input;
    const char *hex;
  };

  const Case cases[] = {
      {"00000000-0000-0000-0000-000000000000", "00000000000000000000000000000000"},
      {"12345678-1234-5678-9abc-def012345678", "12345678123456789abcdef012345678"},
      {"ABCDEF01-2345-6789-ABCD-EF0123456789", "abcdef0123456789abcdef0123456789"},
      {"{12345678-1234-5678-9abc-def012345678}", "12345678123456789abcdef012345678"},
      {"{ABCDEF01-2345-6789-ABCD-EF0123456789}", "abcdef0123456789abcdef0123456789"},
  };

  for (const Case &c : cases) {
    std::array<uint8_t, NEI_UUID_BINARY_SIZE> uuid{};
    ASSERT_EQ(nei_uuid_from_string(c.input, uuid.data()), 0) << c.input;
    EXPECT_EQ(ToHex(uuid.data(), uuid.size()), c.hex) << c.input;
  }
}

TEST(UtilsUuidTest, FromStringRoundTripsWithToString) {
  for (int i = 0; i < 100; ++i) {
    std::array<uint8_t, NEI_UUID_BINARY_SIZE> uuid{};
    std::array<uint8_t, NEI_UUID_BINARY_SIZE> parsed{};
    std::array<char, NEI_UUID_STRING_SIZE> s{};
    ASSERT_GE(nei_uuid4_generate(uuid.data()), 0);
    ASSERT_EQ(nei_uuid_to_string(uuid.data(), s.data()), 0);
    ASSERT_EQ(nei_uuid_from_string(s.data(), parsed.data()), 0);
    EXPECT_EQ(ToHex(uuid.data(), uuid.size()), ToHex(parsed.data(), parsed.size()));
  }
}

TEST(UtilsUuidTest, FromStringRejectsInvalidInput) {
  std::array<uint8_t, NEI_UUID_BINARY_SIZE> uuid{};

  const char *invalid[] = {
      "",
      "not-a-uuid",
      "12345678-1234-5678-9abc-def01234567",
      "12345678-1234-5678-9abc-def0123456789",
      "12345678-1234-5678-9abc-def01234567g",
      "12345678-1234-5678-9abc-def01234567-",
      "{12345678-1234-5678-9abc-def012345678",
      "12345678-1234-5678-9abc-def012345678}",
      "{12345678-1234-5678-9abc-def01234567g}",
  };

  for (const char *s : invalid) {
    EXPECT_EQ(nei_uuid_from_string(s, uuid.data()), NEI_UUID_ERR_INVALID_FORMAT) << s;
  }
  EXPECT_EQ(nei_uuid_from_string(nullptr, uuid.data()), NEI_UUID_ERR_INVALID_ARG);
  EXPECT_EQ(nei_uuid_from_string("12345678-1234-5678-9abc-def012345678", nullptr), NEI_UUID_ERR_INVALID_ARG);
}

TEST(UtilsUuidTest, CompareOrdersLexicographically) {
  std::array<uint8_t, NEI_UUID_BINARY_SIZE> a{};
  std::array<uint8_t, NEI_UUID_BINARY_SIZE> b{};
  ASSERT_EQ(nei_uuid_from_string("00000000-0000-0000-0000-000000000000", a.data()), 0);
  ASSERT_EQ(nei_uuid_from_string("00000000-0000-0000-0000-000000000001", b.data()), 0);

  EXPECT_LT(nei_uuid_compare(a.data(), b.data()), 0);
  EXPECT_GT(nei_uuid_compare(b.data(), a.data()), 0);
  EXPECT_EQ(nei_uuid_compare(a.data(), a.data()), 0);
}

TEST(UtilsUuidTest, CompareAndEqualHandleNullAndCase) {
  std::array<uint8_t, NEI_UUID_BINARY_SIZE> a{};
  std::array<uint8_t, NEI_UUID_BINARY_SIZE> b{};
  std::array<uint8_t, NEI_UUID_BINARY_SIZE> braced{};
  ASSERT_EQ(nei_uuid_from_string("12345678-1234-5678-9abc-def012345678", a.data()), 0);
  ASSERT_EQ(nei_uuid_from_string("12345678-1234-5678-9ABC-DEF012345678", b.data()), 0);
  ASSERT_EQ(nei_uuid_from_string("{12345678-1234-5678-9abc-def012345678}", braced.data()), 0);

  EXPECT_EQ(nei_uuid_compare(nullptr, nullptr), 0);
  EXPECT_LT(nei_uuid_compare(nullptr, a.data()), 0);
  EXPECT_GT(nei_uuid_compare(a.data(), nullptr), 0);

  EXPECT_EQ(nei_uuid_equal(a.data(), b.data()), 1);
  EXPECT_EQ(nei_uuid_equal(a.data(), braced.data()), 1);
  EXPECT_EQ(nei_uuid_equal(nullptr, a.data()), 0);
  EXPECT_EQ(nei_uuid_equal(a.data(), nullptr), 0);
  EXPECT_EQ(nei_uuid_equal(nullptr, nullptr), 0);
}
