// =============================================================================
// gzip_stream.h — incremental gzip/deflate compression primitives
// =============================================================================
//
// Thin streaming wrappers over zlib (3rdparty/zlib) for HTTP content
// encoding.  They are deliberately low-level: they do not know about HTTP
// headers or framing, they just compress/decompress byte streams incrementally
// so both buffered and streaming HTTP bodies can be transformed.
//
// Supported encodings (RFC 1950/1951/1952):
//   - kGzip    RFC 1952 gzip (deflate data + gzip member header/trailer) —
//              what "Content-Encoding: gzip" means on the wire.
//   - kZlib    RFC 1950 zlib wrapper (2-byte header + adler32 trailer).
//   - kDeflate raw DEFLATE stream with no wrapper.  Historic
//              "Content-Encoding: deflate" sometimes means raw deflate
//              (pre-RFC 9110), so we accept both on decode.
//
// Thread safety: each object is single-threaded; create one per stream.
//
// ABI note: implementations live in the .cpp, so the class is a PIMPL and
// stays size-stable as zlib usage evolves.

#ifndef NEIXX_NET_HTTP_GZIP_STREAM_H_
#define NEIXX_NET_HTTP_GZIP_STREAM_H_

#include <cstddef>
#include <memory>
#include <string>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>

namespace nei::net::http {

enum class GzipEncoding {
  kGzip,    // RFC 1952 gzip
  kZlib,    // RFC 1950 zlib wrapper
  kDeflate, // raw DEFLATE (no wrapper)
};

NEI_SUPPRESS_MSC_WARNING_4251_BEGIN

// Incremental decompressor.  Feed compressed bytes via Decompress(); the
// decompressed bytes are appended to the caller's out string (or delivered
// chunk-by-chunk for streaming callers who keep their own buffer).  Call
// Finish() after the final compressed byte to flush the tail and validate
// the stream (gzip trailer checksum, unexpected end, etc.).
class NEI_API GzipDecompressor {
public:
  // |encoding| selects how to interpret the input.  kGzip is the default.
  explicit GzipDecompressor(GzipEncoding encoding = GzipEncoding::kGzip);
  ~GzipDecompressor();
  GzipDecompressor(const GzipDecompressor &) = delete;
  GzipDecompressor &operator=(const GzipDecompressor &) = delete;

  // Consumes |len| compressed bytes, appending any decompressed output to
  // |out|.  Returns false if the input is corrupt or the internal window
  // cannot be allocated.  Safe to call with len == 0 (no-op).
  bool Decompress(const char *data, size_t len, std::string *out);

  // Signals end of compressed input: flushes remaining output and validates
  // the member trailer.  Returns false on a truncated/corrupt stream.
  bool Finish(std::string *out);

  // True once the member end (and trailer) has been fully decoded.  Remains
  // false while more input may still be required.
  bool done() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Incremental compressor.  Feed bytes via Compress(); output (compressed
// bytes) is appended to |out|.  Call Finish() to emit the final block and
// member trailer.
class NEI_API GzipCompressor {
public:
  // |level| is a zlib compression level: 0 = store, 1..9 = faster..best,
  // -1 = default (zlib's Z_DEFAULT_COMPRESSION).
  explicit GzipCompressor(GzipEncoding encoding = GzipEncoding::kGzip, int level = -1);
  ~GzipCompressor();
  GzipCompressor(const GzipCompressor &) = delete;
  GzipCompressor &operator=(const GzipCompressor &) = delete;

  // Compresses |len| bytes, appending compressed output to |out|.
  void Compress(const char *data, size_t len, std::string *out);

  // Emits the final block and trailer.  Must be called exactly once.
  void Finish(std::string *out);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

NEI_SUPPRESS_MSC_WARNING_4251_END

// Compresses |input| in one shot (gzip framing).
std::string NEI_API GzipCompress(const std::string &input, int level = -1);

// Decompresses |input| (gzip framing) in one shot.  Returns false on corrupt
// input.
bool NEI_API GzipDecompress(const std::string &input, std::string *output);

} // namespace nei::net::http

#endif // NEIXX_NET_HTTP_GZIP_STREAM_H_
