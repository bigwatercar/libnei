#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include <nei/utils/base64.h>

TEST(Base64Test, EncodeKnownVectors) {
  struct Case {
    const char *plain;
    const char *encoded;
  };
  const Case cases[] = {
      {"", ""},
      {"f", "Zg=="},
      {"fo", "Zm8="},
      {"foo", "Zm9v"},
      {"foobar", "Zm9vYmFy"},
      {"hello, world", "aGVsbG8sIHdvcmxk"},
  };

  for (const Case &c : cases) {
    const size_t in_len = std::strlen(c.plain);
    std::vector<char> out(nei_base64_encoded_length(in_len));
    size_t out_len = 0U;
    ASSERT_EQ(nei_base64_encode(
                  reinterpret_cast<const uint8_t *>(c.plain), in_len, out.data(), out.size(), &out_len),
              NEI_BASE64_OK);
    ASSERT_EQ(out_len, std::strlen(c.encoded));
    EXPECT_EQ(std::string(out.data(), out_len), std::string(c.encoded));
  }
}

TEST(Base64Test, DecodeKnownVectors) {
  struct Case {
    const char *encoded;
    const char *plain;
  };
  const Case cases[] = {
      {"", ""},
      {"Zg==", "f"},
      {"Zm8=", "fo"},
      {"Zm9v", "foo"},
      {"Zm9vYmFy", "foobar"},
      {"aGVsbG8sIHdvcmxk", "hello, world"},
  };

  for (const Case &c : cases) {
    const size_t in_len = std::strlen(c.encoded);
    std::vector<uint8_t> out(nei_base64_decoded_max_length(in_len));
    size_t out_len = 0U;
    ASSERT_EQ(nei_base64_decode(c.encoded, in_len, out.data(), out.size(), &out_len), NEI_BASE64_OK);
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(out.data()), out_len), std::string(c.plain));
  }
}

TEST(Base64Test, RoundTripBinaryData) {
  std::array<uint8_t, 256> input{};
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = static_cast<uint8_t>(i);
  }

  std::vector<char> b64(nei_base64_encoded_length(input.size()));
  size_t b64_len = 0U;
  ASSERT_EQ(nei_base64_encode(input.data(), input.size(), b64.data(), b64.size(), &b64_len), NEI_BASE64_OK);

  std::vector<uint8_t> decoded(nei_base64_decoded_max_length(b64_len));
  size_t decoded_len = 0U;
  ASSERT_EQ(nei_base64_decode(b64.data(), b64_len, decoded.data(), decoded.size(), &decoded_len), NEI_BASE64_OK);
  ASSERT_EQ(decoded_len, input.size());
  EXPECT_EQ(std::vector<uint8_t>(decoded.begin(), decoded.begin() + decoded_len),
            std::vector<uint8_t>(input.begin(), input.end()));
}

TEST(Base64Test, RejectsInvalidInput) {
  uint8_t out[16] = {0};
  size_t out_len = 0U;

  EXPECT_EQ(nei_base64_decode("abc", 3U, out, sizeof(out), &out_len), NEI_BASE64_ERR_INVALID_INPUT);
  EXPECT_EQ(nei_base64_decode("ab=c", 4U, out, sizeof(out), &out_len), NEI_BASE64_ERR_INVALID_INPUT);
  EXPECT_EQ(nei_base64_decode("a===", 4U, out, sizeof(out), &out_len), NEI_BASE64_ERR_INVALID_INPUT);
  EXPECT_EQ(nei_base64_decode("Zm9v=", 5U, out, sizeof(out), &out_len), NEI_BASE64_ERR_INVALID_INPUT);
}

TEST(Base64Test, ValidatesBufferSizes) {
  char out_b64[3] = {0};
  size_t out_len = 0U;
  EXPECT_EQ(nei_base64_encode(reinterpret_cast<const uint8_t *>("f"), 1U, out_b64, sizeof(out_b64), &out_len),
            NEI_BASE64_ERR_OUTPUT_TOO_SMALL);

  uint8_t out_raw[2] = {0};
  EXPECT_EQ(nei_base64_decode("Zm9v", 4U, out_raw, sizeof(out_raw), &out_len), NEI_BASE64_ERR_OUTPUT_TOO_SMALL);
}
