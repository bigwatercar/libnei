// =============================================================================
// cookie_test — RFC 6265 cookie parsing, matching and jar behaviour
// =============================================================================

#include <neixx/net/http/cookie.h>

#include <gtest/gtest.h>

#include <string>

namespace nei {
namespace net::http {
namespace {

Url UrlFrom(const std::string &spec) {
  return Url(spec);
}

TEST(CookieTest, ParseSimpleSessionCookie) {
  auto c = Cookie::Parse("session=abc123", UrlFrom("https://example.com/"));
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->name, "session");
  EXPECT_EQ(c->value, "abc123");
  EXPECT_EQ(c->domain, "example.com");
  EXPECT_EQ(c->path, "/");
  EXPECT_TRUE(c->host_only);
  EXPECT_FALSE(c->secure);
  EXPECT_FALSE(c->expires.has_value());
}

TEST(CookieTest, ParseWithAttributes) {
  auto c = Cookie::Parse("id=42; Domain=.example.com; Path=/api; Secure; HttpOnly",
                         UrlFrom("https://sub.example.com/api/v1"));
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->name, "id");
  EXPECT_EQ(c->value, "42");
  EXPECT_EQ(c->domain, "example.com"); // leading dot stripped
  EXPECT_FALSE(c->host_only);
  EXPECT_EQ(c->path, "/api");
  EXPECT_TRUE(c->secure);
  EXPECT_TRUE(c->http_only);
}

TEST(CookieTest, DefaultPathFromRequest) {
  auto c = Cookie::Parse("a=1", UrlFrom("https://example.com/dir/page"));
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->path, "/dir");
}

TEST(CookieTest, RejectsDomainMismatch) {
  EXPECT_FALSE(Cookie::Parse("a=1; Domain=other.com", UrlFrom("https://example.com/")).has_value());
}

TEST(CookieTest, ParsesExpiresRfc1123) {
  // "Wed, 09 Jun 2021 10:18:14 GMT" = 1623233894 unix.
  auto c = Cookie::Parse("a=1; Expires=Wed, 09 Jun 2021 10:18:14 GMT", UrlFrom("https://example.com/"));
  ASSERT_TRUE(c.has_value());
  ASSERT_TRUE(c->expires.has_value());
  EXPECT_EQ(c->expires->ToUnixSeconds(), 1623233894);
}

TEST(CookieTest, ParsesMaxAgeAndPrefersIt) {
  auto c = Cookie::Parse("a=1; Max-Age=3600; Expires=Wed, 09 Jun 2021 10:18:14 GMT", UrlFrom("https://example.com/"));
  ASSERT_TRUE(c.has_value());
  ASSERT_TRUE(c->expires.has_value());
  // Max-Age wins over Expires: about now + 3600s.
  const int64_t expected = Time::Now().ToUnixSeconds() + 3600;
  EXPECT_NEAR(c->expires->ToUnixSeconds(), expected, 5);
}

TEST(CookieTest, IsExpired) {
  auto c = Cookie::Parse("a=1; Max-Age=1", UrlFrom("https://example.com/"));
  ASSERT_TRUE(c.has_value());
  // Simulate the past: force an already-past expiry.
  c->expires = Time::FromUnixSeconds(1000);
  EXPECT_TRUE(c->IsExpired(Time::Now()));
}

TEST(CookieTest, DomainMatchSubdomain) {
  auto c = Cookie::Parse("a=1; Domain=example.com", UrlFrom("https://example.com/"));
  ASSERT_TRUE(c.has_value());
  EXPECT_TRUE(c->Matches(UrlFrom("https://www.example.com/x"), Time::Now()));
  EXPECT_TRUE(c->Matches(UrlFrom("https://example.com/x"), Time::Now()));
  EXPECT_FALSE(c->Matches(UrlFrom("https://notexample.com/x"), Time::Now()));
  EXPECT_FALSE(c->Matches(UrlFrom("https://badexample.com/x"), Time::Now()));
}

TEST(CookieTest, HostOnlyCookieNotSentToSubdomain) {
  auto c = Cookie::Parse("a=1", UrlFrom("https://example.com/"));
  ASSERT_TRUE(c.has_value());
  EXPECT_TRUE(c->Matches(UrlFrom("https://example.com/x"), Time::Now()));
  EXPECT_FALSE(c->Matches(UrlFrom("https://sub.example.com/x"), Time::Now()));
}

TEST(CookieTest, PathMatch) {
  auto c = Cookie::Parse("a=1; Path=/api", UrlFrom("https://example.com/api"));
  ASSERT_TRUE(c.has_value());
  EXPECT_TRUE(c->Matches(UrlFrom("https://example.com/api/users"), Time::Now()));
  EXPECT_TRUE(c->Matches(UrlFrom("https://example.com/api"), Time::Now()));
  EXPECT_FALSE(c->Matches(UrlFrom("https://example.com/apix"), Time::Now()));
  EXPECT_FALSE(c->Matches(UrlFrom("https://example.com/"), Time::Now()));
}

TEST(CookieTest, SecureCookieOnlyOverHttps) {
  auto c = Cookie::Parse("a=1; Secure", UrlFrom("https://example.com/"));
  ASSERT_TRUE(c.has_value());
  EXPECT_TRUE(c->Matches(UrlFrom("https://example.com/"), Time::Now()));
  EXPECT_FALSE(c->Matches(UrlFrom("http://example.com/"), Time::Now()));
}

TEST(CookieJarTest, SetAndGet) {
  CookieJar jar;
  auto c = Cookie::Parse("session=abc", UrlFrom("https://example.com/login"));
  ASSERT_TRUE(c.has_value());
  jar.SetCookie(*c);
  EXPECT_EQ(jar.size(), 1u);
  EXPECT_EQ(jar.GetCookieHeader(UrlFrom("https://example.com/")), "session=abc");
  // Wrong host: no cookie.
  EXPECT_TRUE(jar.GetCookieHeader(UrlFrom("https://other.com/")).empty());
}

TEST(CookieJarTest, ReplacesSameNameDomainPath) {
  CookieJar jar;
  auto c1 = Cookie::Parse("id=1", UrlFrom("https://example.com/"));
  ASSERT_TRUE(c1.has_value());
  jar.SetCookie(*c1);
  auto c2 = Cookie::Parse("id=2", UrlFrom("https://example.com/"));
  ASSERT_TRUE(c2.has_value());
  jar.SetCookie(*c2);
  EXPECT_EQ(jar.size(), 1u);
  EXPECT_EQ(jar.GetCookieHeader(UrlFrom("https://example.com/")), "id=2");
}

TEST(CookieJarTest, MultipleCookiesCombined) {
  CookieJar jar;
  auto c1 = Cookie::Parse("a=1; Path=/", UrlFrom("https://example.com/"));
  auto c2 = Cookie::Parse("b=2; Path=/api", UrlFrom("https://example.com/api"));
  ASSERT_TRUE(c1.has_value());
  ASSERT_TRUE(c2.has_value());
  jar.SetCookie(*c1);
  jar.SetCookie(*c2);
  const std::string header = jar.GetCookieHeader(UrlFrom("https://example.com/api/x"));
  EXPECT_NE(header.find("a=1"), std::string::npos);
  EXPECT_NE(header.find("b=2"), std::string::npos);
}

TEST(CookieJarTest, DropsExpiredOnSet) {
  CookieJar jar;
  auto c = Cookie::Parse("a=1; Max-Age=1", UrlFrom("https://example.com/"));
  ASSERT_TRUE(c.has_value());
  c->expires = Time::UnixEpoch(); // force expired
  jar.SetCookie(*c);
  EXPECT_EQ(jar.size(), 0u);
}

} // namespace
} // namespace net::http
} // namespace nei
