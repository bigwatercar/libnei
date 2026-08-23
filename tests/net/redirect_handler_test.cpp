// =============================================================================
// redirect_handler_test — HTTP 3xx redirect decision logic
// =============================================================================

#include <neixx/net/http/redirect_handler.h>

#include <gtest/gtest.h>

#include <string>

namespace nei {
namespace net::http {
namespace {

HttpResponse MakeRedirect(int status, const std::string &location) {
  HttpResponse resp;
  resp.SetStatus(static_cast<HttpStatusCode>(status));
  if (!location.empty())
    resp.headers.push_back({"Location", location});
  return resp;
}

TEST(RedirectHandlerTest, NonRedirectStatusReturnsNullopt) {
  HttpResponse resp;
  resp.SetStatus(HttpStatusCode::kOk);
  EXPECT_FALSE(ComputeRedirect(resp, Url("https://example.com/"), HttpMethod::kGet, 5).has_value());
}

TEST(RedirectHandlerTest, RedirectWithoutLocationReturnsNullopt) {
  EXPECT_FALSE(ComputeRedirect(MakeRedirect(302, ""), Url("https://example.com/"), HttpMethod::kGet, 5).has_value());
}

TEST(RedirectHandlerTest, NoRemainingHopsReturnsNullopt) {
  EXPECT_FALSE(
      ComputeRedirect(MakeRedirect(302, "/next"), Url("https://example.com/"), HttpMethod::kGet, 0).has_value());
  EXPECT_FALSE(
      ComputeRedirect(MakeRedirect(302, "/next"), Url("https://example.com/"), HttpMethod::kGet, -1).has_value());
}

TEST(RedirectHandlerTest, AbsoluteLocationKept) {
  auto d = ComputeRedirect(
      MakeRedirect(302, "https://other.com/path?q=1"), Url("https://example.com/"), HttpMethod::kGet, 5);
  ASSERT_TRUE(d.has_value());
  EXPECT_TRUE(d->follow);
  EXPECT_EQ(std::string(d->target.host()), "other.com");
  EXPECT_EQ(std::string(d->target.path()), "/path");
}

TEST(RedirectHandlerTest, RelativeLocationResolved) {
  // Path-relative.
  auto d = ComputeRedirect(MakeRedirect(301, "/next"), Url("https://example.com/a/b"), HttpMethod::kGet, 5);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(std::string(d->target.path()), "/next");
  // Relative with .. .
  d = ComputeRedirect(MakeRedirect(301, "../up"), Url("https://example.com/a/b/c"), HttpMethod::kGet, 5);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(std::string(d->target.path()), "/a/up");
  // Fragment-only keeps the path.
  d = ComputeRedirect(MakeRedirect(301, "#frag"), Url("https://example.com/a/b"), HttpMethod::kGet, 5);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(std::string(d->target.path()), "/a/b");
}

TEST(RedirectHandlerTest, GetStaysGet) {
  auto d = ComputeRedirect(MakeRedirect(302, "/next"), Url("https://example.com/"), HttpMethod::kGet, 5);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->method, HttpMethod::kGet);
  EXPECT_FALSE(d->method_changed);
}

TEST(RedirectHandlerTest, PostRewrittenToGetOn302) {
  auto d = ComputeRedirect(MakeRedirect(302, "/next"), Url("https://example.com/"), HttpMethod::kPost, 5);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->method, HttpMethod::kGet);
  EXPECT_TRUE(d->method_changed);
}

TEST(RedirectHandlerTest, HeadStaysHeadOn303) {
  auto d = ComputeRedirect(MakeRedirect(303, "/next"), Url("https://example.com/"), HttpMethod::kHead, 5);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->method, HttpMethod::kHead);
  EXPECT_FALSE(d->method_changed);
}

TEST(RedirectHandlerTest, PostPreservedOn307) {
  auto d = ComputeRedirect(MakeRedirect(307, "/next"), Url("https://example.com/"), HttpMethod::kPost, 5);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->method, HttpMethod::kPost);
  EXPECT_FALSE(d->method_changed);
}

TEST(RedirectHandlerTest, PostPreservedOn308) {
  auto d = ComputeRedirect(MakeRedirect(308, "/next"), Url("https://example.com/"), HttpMethod::kPost, 5);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->method, HttpMethod::kPost);
  EXPECT_FALSE(d->method_changed);
}

TEST(RedirectHandlerTest, InvalidLocationRejected) {
  // Location with no host cannot be followed.
  auto d = ComputeRedirect(MakeRedirect(302, "http://"), Url("https://example.com/"), HttpMethod::kGet, 5);
  EXPECT_FALSE(d.has_value());
  // Non-http(s) scheme is rejected too.
  d = ComputeRedirect(MakeRedirect(302, "ftp://example.com/x"), Url("https://example.com/"), HttpMethod::kGet, 5);
  EXPECT_FALSE(d.has_value());
}

} // namespace
} // namespace net::http
} // namespace nei
