// =============================================================================
// Url unit tests — RFC 3986 parsing, encoding, and resolution
// =============================================================================

#include <gtest/gtest.h>

#include <neixx/url/url.h>
#include <neixx/url/url_encoding.h>

#include <sstream>

namespace nei {
namespace {

// =============================================================================
// Basic parsing
// =============================================================================

TEST(UrlTest, EmptyUrl) {
  Url u("");
  EXPECT_TRUE(u.is_empty());
  EXPECT_FALSE(u.is_valid()); // empty is not a valid URL
  EXPECT_EQ(u.spec(), "");
}

TEST(UrlTest, HttpFull) {
  Url u("https://user:pass@example.com:8080/path/to?q=1&k=v#section");
  EXPECT_TRUE(u.is_valid());
  EXPECT_EQ(u.scheme(), "https");
  EXPECT_EQ(u.user(), "user");
  EXPECT_EQ(u.password(), "pass");
  EXPECT_EQ(u.host(), "example.com");
  EXPECT_EQ(u.port(), 8080);
  EXPECT_EQ(u.path(), "/path/to");
  EXPECT_EQ(u.query(), "q=1&k=v");
  EXPECT_EQ(u.fragment(), "section");
}

TEST(UrlTest, HttpMinimal) {
  Url u("http://example.com");
  EXPECT_TRUE(u.is_valid());
  EXPECT_EQ(u.scheme(), "http");
  EXPECT_EQ(u.host(), "example.com");
  EXPECT_EQ(u.port(), 80); // default
  EXPECT_EQ(u.path(), "");
}

TEST(UrlTest, HttpPathOnly) {
  Url u("https://example.com/path");
  EXPECT_EQ(u.path(), "/path");
}

TEST(UrlTest, HttpQueryOnly) {
  Url u("https://example.com?a=1");
  EXPECT_EQ(u.path(), "");
  EXPECT_EQ(u.query(), "a=1");
}

TEST(UrlTest, HttpFragmentOnly) {
  Url u("https://example.com#top");
  EXPECT_EQ(u.fragment(), "top");
}

// =============================================================================
// IPv6
// =============================================================================

TEST(UrlTest, Ipv6Literal) {
  Url u("https://[::1]:8080/path");
  EXPECT_TRUE(u.is_valid());
  EXPECT_EQ(u.host(), "[::1]");
  EXPECT_EQ(u.port(), 8080);
  EXPECT_EQ(u.path(), "/path");
}

TEST(UrlTest, Ipv6LoopbackDefaultPort) {
  Url u("http://[::1]/");
  EXPECT_EQ(u.host(), "[::1]");
  EXPECT_EQ(u.port(), 80);
}

// =============================================================================
// Scheme normalization
// =============================================================================

TEST(UrlTest, SchemeToLower) {
  Url u("HTTPS://Example.COM");
  EXPECT_EQ(u.scheme(), "https");
  EXPECT_EQ(u.host(), "example.com");
}

// =============================================================================
// Origin
// =============================================================================

TEST(UrlTest, OriginDefaultPort) {
  Url u("https://example.com/path");
  EXPECT_EQ(u.origin(), "https://example.com");
}

TEST(UrlTest, OriginNonDefaultPort) {
  Url u("http://example.com:8080/path");
  EXPECT_EQ(u.origin(), "http://example.com:8080");
}

// =============================================================================
// ws:// and wss://
// =============================================================================

TEST(UrlTest, WebsocketScheme) {
  Url ws("ws://example.com/chat");
  EXPECT_EQ(ws.scheme(), "ws");
  EXPECT_EQ(ws.port(), 80);

  Url wss("wss://example.com/chat");
  EXPECT_EQ(wss.scheme(), "wss");
  EXPECT_EQ(wss.port(), 443);
}

// =============================================================================
// Copy / move
// =============================================================================

TEST(UrlTest, CopyConstructor) {
  Url a("https://a.com");
  Url b(a);
  EXPECT_EQ(a.spec(), b.spec());
  EXPECT_EQ(a.scheme(), b.scheme());
}

TEST(UrlTest, MoveConstructor) {
  Url a("https://a.com");
  std::string spec = a.spec();
  Url b(std::move(a));
  EXPECT_EQ(b.spec(), spec);
}

TEST(UrlTest, CopyAssignment) {
  Url a("https://a.com"), b("https://b.com");
  b = a;
  EXPECT_EQ(b.spec(), "https://a.com");
}

// =============================================================================
// Relative reference resolution (RFC 3986 §5)
// =============================================================================

TEST(UrlTest, ResolveAbsolutePath) {
  Url base("https://example.com/base/index.html");
  Url resolved = base.Resolve("/absolute/path");
  EXPECT_EQ(resolved.spec(), "https://example.com/absolute/path");
}

TEST(UrlTest, ResolveRelativePath) {
  Url base("https://example.com/base/index.html");
  Url resolved = base.Resolve("other.html");
  EXPECT_EQ(resolved.spec(), "https://example.com/base/other.html");
}

TEST(UrlTest, ResolveParentDir) {
  Url base("https://example.com/a/b/c.html");
  Url resolved = base.Resolve("../d.html");
  EXPECT_EQ(resolved.spec(), "https://example.com/a/d.html");
}

TEST(UrlTest, ResolveQueryOnly) {
  Url base("https://example.com/path");
  Url resolved = base.Resolve("?new=query");
  EXPECT_EQ(resolved.spec(), "https://example.com/path?new=query");
}

TEST(UrlTest, ResolveFragmentOnly) {
  Url base("https://example.com/path");
  Url resolved = base.Resolve("#section");
  EXPECT_EQ(resolved.spec(), "https://example.com/path#section");
}

TEST(UrlTest, ResolveSchemeAbsolute) {
  Url base("https://example.com/path");
  Url resolved = base.Resolve("http://other.com/");
  EXPECT_EQ(resolved.spec(), "http://other.com/");
}

// =============================================================================
// Comparison / streaming
// =============================================================================

TEST(UrlTest, Equality) {
  Url a("https://a.com"), b("https://a.com"), c("https://b.com");
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

TEST(UrlTest, Ostream) {
  Url u("https://x.com");
  std::ostringstream oss;
  oss << u;
  EXPECT_EQ(oss.str(), "https://x.com");
}

// =============================================================================
// Percent encoding / decoding
// =============================================================================

TEST(UrlEncodingTest, EncodeUnreserved) {
  EXPECT_EQ(UrlEncode("abcXYZ123-._~"), "abcXYZ123-._~");
}

TEST(UrlEncodingTest, EncodeSpace) {
  EXPECT_EQ(UrlEncode("hello world"), "hello%20world");
}

TEST(UrlEncodingTest, EncodeQuerySpace) {
  EXPECT_EQ(UrlEncodeQuery("hello world"), "hello+world");
}

TEST(UrlEncodingTest, EncodeSpecialChars) {
  EXPECT_EQ(UrlEncode("/?=&"), "%2F%3F%3D%26");
}

TEST(UrlEncodingTest, DecodeBasic) {
  EXPECT_EQ(UrlDecode("hello%20world"), "hello world");
}

TEST(UrlEncodingTest, DecodePlus) {
  EXPECT_EQ(UrlDecode("hello+world"), "hello world");
}

TEST(UrlEncodingTest, DecodeSpecialChars) {
  EXPECT_EQ(UrlDecode("%2F%3F%3D%26"), "/?=&");
}

TEST(UrlEncodingTest, DecodeInvalidSeq) {
  EXPECT_EQ(UrlDecode("%GG"), "%GG"); // not valid hex
  EXPECT_EQ(UrlDecode("%2"), "%2");   // truncated
}

TEST(UrlEncodingTest, RoundTrip) {
  std::string original = "hello world!@#$%^&*()";
  std::string encoded = UrlEncode(original);
  std::string decoded = UrlDecode(encoded);
  EXPECT_EQ(decoded, original);
}

} // namespace
} // namespace nei
