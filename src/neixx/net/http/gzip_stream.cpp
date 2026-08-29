// =============================================================================
// gzip_stream.cpp — incremental gzip/deflate compression primitives (zlib)
// =============================================================================

#include <neixx/net/http/gzip_stream.h>

#include <zlib.h>

#include <cstring>

namespace nei::net::http {

namespace {

constexpr std::size_t kChunkSize = 16 * 1024;

int WindowBitsFor(GzipEncoding encoding) {
  switch (encoding) {
  case GzipEncoding::kGzip:
    return 15 + 16; // gzip wrapper
  case GzipEncoding::kZlib:
    return 15; // zlib wrapper
  case GzipEncoding::kDeflate:
    return -15; // raw deflate
  }
  return 15 + 16;
}

} // namespace

// ---------------------------------------------------------------------------
// GzipDecompressor
// ---------------------------------------------------------------------------
struct GzipDecompressor::Impl {
  z_stream zs = {};
  bool initialized = false;
  bool done_ = false;
  bool failed = false;
  GzipEncoding encoding;
};

GzipDecompressor::GzipDecompressor(GzipEncoding encoding)
    : impl_(new Impl) {
  impl_->encoding = encoding;
  std::memset(&impl_->zs, 0, sizeof(impl_->zs));
  const int window_bits = WindowBitsFor(encoding);
  const int rc = inflateInit2(&impl_->zs, window_bits);
  impl_->initialized = (rc == Z_OK);
  if (!impl_->initialized)
    impl_->failed = true;
}

GzipDecompressor::~GzipDecompressor() {
  if (impl_ && impl_->initialized)
    inflateEnd(&impl_->zs);
}

bool GzipDecompressor::Decompress(const char *data, size_t len, std::string *out) {
  if (impl_->failed || impl_->done_)
    return !impl_->failed;
  if (len == 0)
    return true;

  impl_->zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data));
  impl_->zs.avail_in = static_cast<uInt>(len);

  std::string chunk(kChunkSize, '\0');
  int rc = Z_OK;
  do {
    impl_->zs.next_out = reinterpret_cast<Bytef *>(&chunk[0]);
    impl_->zs.avail_out = static_cast<uInt>(chunk.size());
    rc = inflate(&impl_->zs, Z_NO_FLUSH);
    const std::size_t produced = chunk.size() - impl_->zs.avail_out;
    if (produced > 0)
      out->append(chunk.data(), produced);
    if (rc == Z_STREAM_END) {
      impl_->done_ = true;
      break;
    }
    if (rc != Z_OK && rc != Z_BUF_ERROR) {
      impl_->failed = true;
      return false;
    }
  } while (impl_->zs.avail_out == 0);

  return true;
}

bool GzipDecompressor::Finish(std::string *out) {
  // Process any remaining buffered input; a well-formed stream ends with
  // Z_STREAM_END (gzip trailer validated).  Z_BUF_ERROR here means the input
  // ended before the member trailer — truncated/corrupt.
  if (impl_->failed)
    return false;
  if (!impl_->done_) {
    // One more inflate with no new input to surface the trailer / EOF.
    std::string chunk(kChunkSize, '\0');
    impl_->zs.next_out = reinterpret_cast<Bytef *>(&chunk[0]);
    impl_->zs.avail_out = static_cast<uInt>(chunk.size());
    const int rc = inflate(&impl_->zs, Z_FINISH);
    const std::size_t produced = chunk.size() - impl_->zs.avail_out;
    if (produced > 0)
      out->append(chunk.data(), produced);
    if (rc != Z_STREAM_END) {
      impl_->failed = true;
      return false;
    }
    impl_->done_ = true;
  }
  return true;
}

bool GzipDecompressor::done() const {
  return impl_->done_;
}

// ---------------------------------------------------------------------------
// GzipCompressor
// ---------------------------------------------------------------------------
struct GzipCompressor::Impl {
  z_stream zs = {};
  bool initialized = false;
  bool finished = false;
  GzipEncoding encoding;
  int level;
};

GzipCompressor::GzipCompressor(GzipEncoding encoding, int level)
    : impl_(new Impl) {
  impl_->encoding = encoding;
  impl_->level = level;
  std::memset(&impl_->zs, 0, sizeof(impl_->zs));
  const int window_bits = WindowBitsFor(encoding);
  // Default memLevel/memUsage for compatibility with the HTTP ecosystem.
  const int rc = deflateInit2(&impl_->zs, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY);
  impl_->initialized = (rc == Z_OK);
}

GzipCompressor::~GzipCompressor() {
  if (impl_ && impl_->initialized)
    deflateEnd(&impl_->zs);
}

void GzipCompressor::Compress(const char *data, size_t len, std::string *out) {
  if (!impl_->initialized || impl_->finished || len == 0)
    return;
  impl_->zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data));
  impl_->zs.avail_in = static_cast<uInt>(len);

  std::string chunk(kChunkSize, '\0');
  do {
    impl_->zs.next_out = reinterpret_cast<Bytef *>(&chunk[0]);
    impl_->zs.avail_out = static_cast<uInt>(chunk.size());
    const int rc = deflate(&impl_->zs, Z_NO_FLUSH);
    const std::size_t produced = chunk.size() - impl_->zs.avail_out;
    if (produced > 0)
      out->append(chunk.data(), produced);
    (void)rc; // Z_OK / Z_BUF_ERROR are both expected during compression.
  } while (impl_->zs.avail_out == 0);
}

void GzipCompressor::Finish(std::string *out) {
  if (!impl_->initialized || impl_->finished)
    return;
  impl_->finished = true;
  impl_->zs.next_in = nullptr;
  impl_->zs.avail_in = 0;

  std::string chunk(kChunkSize, '\0');
  int rc = Z_OK;
  do {
    impl_->zs.next_out = reinterpret_cast<Bytef *>(&chunk[0]);
    impl_->zs.avail_out = static_cast<uInt>(chunk.size());
    rc = deflate(&impl_->zs, Z_FINISH);
    const std::size_t produced = chunk.size() - impl_->zs.avail_out;
    if (produced > 0)
      out->append(chunk.data(), produced);
  } while (rc != Z_STREAM_END);
}

// ---------------------------------------------------------------------------
// One-shot helpers
// ---------------------------------------------------------------------------
std::string GzipCompress(const std::string &input, int level) {
  GzipCompressor compressor(GzipEncoding::kGzip, level);
  std::string output;
  compressor.Compress(input.data(), input.size(), &output);
  compressor.Finish(&output);
  return output;
}

bool GzipDecompress(const std::string &input, std::string *output) {
  if (!output)
    return false;
  GzipDecompressor decompressor(GzipEncoding::kGzip);
  output->clear();
  if (!decompressor.Decompress(input.data(), input.size(), output))
    return false;
  return decompressor.Finish(output);
}

} // namespace nei::net::http
