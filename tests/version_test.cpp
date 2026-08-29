#include <gtest/gtest.h>

#include <nei/build/version.h>
#include <nei/core/version.h>

#include <string>

namespace nei {
namespace {

// The generated compile-time macros must agree with the runtime API.
TEST(VersionTest, MacrosMatchRuntimeApi) {
  nei_version_info info = nei_get_version_info();
  EXPECT_EQ(info.major, static_cast<unsigned int>(NEI_VERSION_MAJOR));
  EXPECT_EQ(info.minor, static_cast<unsigned int>(NEI_VERSION_MINOR));
  EXPECT_EQ(info.patch, static_cast<unsigned int>(NEI_VERSION_PATCH));
  EXPECT_EQ(std::string(nei_get_version_string()), std::string(NEI_VERSION_STRING));
}

// The runtime string must be a well-formed major.minor.patch triple.
TEST(VersionTest, StringIsSemanticVersion) {
  const std::string version = nei_get_version_string();
  const std::size_t first_dot = version.find('.');
  const std::size_t second_dot = version.rfind('.');
  EXPECT_NE(first_dot, std::string::npos);
  EXPECT_NE(second_dot, std::string::npos);
  EXPECT_LT(first_dot, second_dot);
  for (char c : version) {
    EXPECT_TRUE((c >= '0' && c <= '9') || c == '.');
  }
}

// Baseline for the first public pre-release (0.9.x).  Bump together with the
// project() VERSION in the root CMakeLists.txt.
TEST(VersionTest, ReportsZeroNineSeries) {
  nei_version_info info = nei_get_version_info();
  EXPECT_EQ(info.major, 0u);
  EXPECT_EQ(info.minor, 9u);
}

} // namespace
} // namespace nei
