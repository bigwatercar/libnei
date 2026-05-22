#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <nei/utils/crc32.h>
#include <nei/utils/sha256.h>

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

std::filesystem::path MakeTempFilePath(const char *name_hint) {
  const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::filesystem::path p = std::filesystem::temp_directory_path();
  p /= std::string("nei_") + name_hint + "_" + std::to_string(static_cast<long long>(ticks)) + ".bin";
  return p;
}

void WriteFileAll(const std::filesystem::path &path, const std::string &content) {
  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
}

}  // namespace

TEST(UtilsCrc32Test, MatchesKnownVectors) {
  struct Case {
    const char *input;
    uint32_t checksum;
    const char *hex;
  };
  const Case cases[] = {
      {"", 0x00000000U, "00000000"},
      {"123456789", 0xCBF43926U, "cbf43926"},
      {"The quick brown fox jumps over the lazy dog", 0x414FA339U, "414fa339"},
  };

  for (const Case &c : cases) {
    EXPECT_EQ(nei_crc32_sum(c.input, std::strlen(c.input)), c.checksum);

    std::array<char, NEI_CRC32_HEX_SIZE> hex{};
    ASSERT_EQ(nei_crc32_sum_hex(c.input, std::strlen(c.input), hex.data()), 0);
    EXPECT_STREQ(hex.data(), c.hex);
  }
}

TEST(UtilsCrc32Test, IncrementalAndFileHelpersWork) {
  const std::string input = "abc";
  const auto path = MakeTempFilePath("crc32");
  nei_crc32_ctx_st ctx{};
  std::array<char, NEI_CRC32_HEX_SIZE> hex{};
  uint32_t checksum = 0U;

  nei_crc32_init(&ctx);
  nei_crc32_update(&ctx, input.data(), 1U);
  nei_crc32_update(&ctx, input.data() + 1, input.size() - 1U);
  checksum = nei_crc32_final(&ctx);
  EXPECT_EQ(checksum, 0x352441C2U);

  nei_crc32_to_hex(checksum, hex.data());
  EXPECT_STREQ(hex.data(), "352441c2");

  WriteFileAll(path, input);
  EXPECT_EQ(nei_crc32_file_sum(path.string().c_str(), &checksum), 0);
  EXPECT_EQ(checksum, 0x352441C2U);
  EXPECT_EQ(nei_crc32_file_sum_hex(path.string().c_str(), hex.data()), 0);
  EXPECT_STREQ(hex.data(), "352441c2");
  std::filesystem::remove(path);
}

TEST(UtilsCrc32Test, RejectsInvalidArgumentsForHelpers) {
  std::array<char, NEI_CRC32_HEX_SIZE> hex{};
  EXPECT_EQ(nei_crc32_sum_hex(nullptr, 3U, hex.data()), -1);
  EXPECT_EQ(nei_crc32_sum_hex("abc", 3U, nullptr), -1);
  EXPECT_EQ(nei_crc32_file_sum(nullptr, nullptr), -1);
}

TEST(UtilsSha256Test, MatchesKnownVectors) {
  struct Case {
    const char *input;
    const char *hex;
  };
  const Case cases[] = {
      {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {"The quick brown fox jumps over the lazy dog",
       "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb7625e0cde6fbb8e592"},
  };

  for (const Case &c : cases) {
    std::array<uint8_t, NEI_SHA256_DIGEST_SIZE> digest{};
    nei_sha256_sum(c.input, std::strlen(c.input), digest.data());
    EXPECT_EQ(ToHex(digest.data(), digest.size()), c.hex);
  }
}

TEST(UtilsSha256Test, IncrementalAndFileHelpersWork) {
  const std::string input = "abc";
  const auto path = MakeTempFilePath("sha256");
  nei_sha256_ctx_st ctx{};
  std::array<uint8_t, NEI_SHA256_DIGEST_SIZE> digest{};
  std::array<char, NEI_SHA256_HEX_SIZE> hex{};

  nei_sha256_init(&ctx);
  nei_sha256_update(&ctx, input.data(), 1U);
  nei_sha256_update(&ctx, input.data() + 1, input.size() - 1U);
  nei_sha256_final(&ctx, digest.data());
  EXPECT_EQ(ToHex(digest.data(), digest.size()),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  nei_sha256_to_hex(digest.data(), hex.data());
  EXPECT_STREQ(hex.data(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  WriteFileAll(path, input);
  EXPECT_EQ(nei_sha256_file_sum_hex(path.string().c_str(), hex.data()), 0);
  EXPECT_STREQ(hex.data(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  std::filesystem::remove(path);
}

TEST(UtilsSha256Test, RejectsInvalidArgumentsForHelpers) {
  std::array<char, NEI_SHA256_HEX_SIZE> hex{};
  EXPECT_EQ(nei_sha256_sum_hex(nullptr, 3U, hex.data()), -1);
  EXPECT_EQ(nei_sha256_sum_hex("abc", 3U, nullptr), -1);
  EXPECT_EQ(nei_sha256_file_sum(nullptr, nullptr), -1);
}