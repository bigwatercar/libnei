// =============================================================================
// gzip_stream_test — incremental gzip/deflate compression primitives
// =============================================================================

#include <neixx/net/http/gzip_stream.h>

#include <gtest/gtest.h>

#include <string>

namespace nei {
namespace net::http {
namespace {

const std::string kCompressible = "the quick brown fox jumps over the lazy dog, repeated for compression "
                                  "the quick brown fox jumps over the lazy dog, repeated for compression ";

TEST(GzipStreamTest, OneShotGzipRoundTrip) {
  const std::string compressed = GzipCompress(kCompressible);
  ASSERT_GT(compressed.size(), 0u);
  EXPECT_LT(compressed.size(), kCompressible.size());
  std::string out;
  ASSERT_TRUE(GzipDecompress(compressed, &out));
  EXPECT_EQ(out, kCompressible);
}

TEST(GzipStreamTest, EmptyRoundTrip) {
  const std::string compressed = GzipCompress("");
  std::string out;
  ASSERT_TRUE(GzipDecompress(compressed, &out));
  EXPECT_TRUE(out.empty());
}

TEST(GzipStreamTest, IncrementalRoundTripGzip) {
  const std::string payload = [&]() {
    std::string s;
    s.reserve(256 * 1024);
    for (int i = 0; i < 4096; ++i)
      s += kCompressible;
    return s;
  }();

  GzipCompressor compressor;
  std::string compressed;
  constexpr std::size_t kFeed = 137; // odd chunk sizes exercise buffering
  std::size_t off = 0;
  while (off < payload.size()) {
    const std::size_t n = std::min(kFeed, payload.size() - off);
    compressor.Compress(payload.data() + off, n, &compressed);
    off += n;
  }
  compressor.Finish(&compressed);
  ASSERT_GT(compressed.size(), 0u);
  EXPECT_LT(compressed.size(), payload.size());

  GzipDecompressor decompressor;
  std::string out;
  off = 0;
  while (off < compressed.size()) {
    const std::size_t n = std::min(kFeed, compressed.size() - off);
    ASSERT_TRUE(decompressor.Decompress(compressed.data() + off, n, &out));
    off += n;
  }
  ASSERT_TRUE(decompressor.Finish(&out));
  EXPECT_TRUE(decompressor.done());
  EXPECT_EQ(out, payload);
}

TEST(GzipStreamTest, RawDeflateRoundTrip) {
  GzipCompressor c(GzipEncoding::kDeflate);
  std::string compressed;
  c.Compress(kCompressible.data(), kCompressible.size(), &compressed);
  c.Finish(&compressed);

  GzipDecompressor d(GzipEncoding::kDeflate);
  std::string out;
  ASSERT_TRUE(d.Decompress(compressed.data(), compressed.size(), &out));
  ASSERT_TRUE(d.Finish(&out));
  EXPECT_EQ(out, kCompressible);
}

TEST(GzipStreamTest, ZlibWrapperRoundTrip) {
  GzipCompressor c(GzipEncoding::kZlib);
  std::string compressed;
  c.Compress(kCompressible.data(), kCompressible.size(), &compressed);
  c.Finish(&compressed);

  GzipDecompressor d(GzipEncoding::kZlib);
  std::string out;
  ASSERT_TRUE(d.Decompress(compressed.data(), compressed.size(), &out));
  ASSERT_TRUE(d.Finish(&out));
  EXPECT_EQ(out, kCompressible);
}

TEST(GzipStreamTest, CorruptInputFails) {
  const std::string compressed = GzipCompress(kCompressible);
  ASSERT_GT(compressed.size(), 2u);
  std::string bad = compressed;
  // Flip bytes in the deflate payload (after the 10-byte gzip header).
  for (std::size_t i = 12; i < bad.size(); ++i)
    bad[i] = static_cast<char>(bad[i] ^ 0xFF);
  std::string out;
  GzipDecompressor d;
  EXPECT_FALSE(d.Decompress(bad.data(), bad.size(), &out));
}

TEST(GzipStreamTest, TruncatedStreamFailsOnFinish) {
  const std::string compressed = GzipCompress(kCompressible);
  ASSERT_GT(compressed.size(), 10u);
  const std::string truncated(compressed.data(), compressed.size() - 5);
  std::string out;
  GzipDecompressor d;
  ASSERT_TRUE(d.Decompress(truncated.data(), truncated.size(), &out));
  // The gzip trailer is missing → Finish must reject.
  EXPECT_FALSE(d.Finish(&out));
}

TEST(GzipStreamTest, CompressionLevel0IsStore) {
  GzipCompressor c(GzipEncoding::kGzip, 0);
  std::string compressed;
  c.Compress(kCompressible.data(), kCompressible.size(), &compressed);
  c.Finish(&compressed);
  // Level 0 stores (may be slightly larger due to framing); still decodable.
  EXPECT_GE(compressed.size(), kCompressible.size());
  std::string out;
  ASSERT_TRUE(GzipDecompress(compressed, &out));
  EXPECT_EQ(out, kCompressible);
}

} // namespace
} // namespace net::http
} // namespace nei
