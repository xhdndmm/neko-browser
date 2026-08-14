// Unit tests for neko::security::Origin (the Same-Origin Policy basis).

#include "neko/security/origin.h"
#include "neko/url/url.h"

#include <gtest/gtest.h>

namespace neko::security {
namespace {

TEST(OriginTest, SameOriginHttpDefaultPort)
{
  auto a = url::Url::Parse("http://example.com/page");
  auto b = url::Url::Parse("http://example.com/other");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_TRUE(Origin::FromUrl(a.value()).IsSameOrigin(Origin::FromUrl(b.value())));
}

TEST(OriginTest, DifferentHostIsDifferentOrigin)
{
  auto a = url::Url::Parse("https://example.com/");
  auto b = url::Url::Parse("https://other.com/");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_FALSE(Origin::FromUrl(a.value()).IsSameOrigin(Origin::FromUrl(b.value())));
}

TEST(OriginTest, DifferentSchemeIsDifferentOrigin)
{
  auto a = url::Url::Parse("http://example.com/");
  auto b = url::Url::Parse("https://example.com/");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_FALSE(Origin::FromUrl(a.value()).IsSameOrigin(Origin::FromUrl(b.value())));
}

TEST(OriginTest, ExplicitPortChangesOrigin)
{
  auto a = url::Url::Parse("http://example.com:8080/");
  auto b = url::Url::Parse("http://example.com/");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_FALSE(Origin::FromUrl(a.value()).IsSameOrigin(Origin::FromUrl(b.value())));
}

TEST(OriginTest, DefaultPortAndExplicitDefaultAreSameOrigin)
{
  // http://example.com and http://example.com:80 are the same origin.
  auto a = url::Url::Parse("http://example.com/");
  auto b = url::Url::Parse("http://example.com:80/");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_TRUE(Origin::FromUrl(a.value()).IsSameOrigin(Origin::FromUrl(b.value())));
}

TEST(OriginTest, SerializeOmitsDefaultPort)
{
  auto a = url::Url::Parse("https://example.com/");
  auto b = url::Url::Parse("https://example.com:8443/");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(Origin::FromUrl(a.value()).Serialize(), "https://example.com");
  EXPECT_EQ(Origin::FromUrl(b.value()).Serialize(), "https://example.com:8443");
}

TEST(OriginTest, OpaqueOriginNeverSameOrigin)
{
  const Origin opaque = Origin::Opaque();
  EXPECT_TRUE(opaque.IsOpaque());
  EXPECT_FALSE(opaque.IsSameOrigin(opaque));
  EXPECT_EQ(opaque.Serialize(), "null");
}

TEST(OriginTest, NonSpecialSchemeIsOpaque)
{
  auto data = url::Url::Parse("data:text/plain,hi");
  ASSERT_TRUE(data.has_value());
  const Origin origin = Origin::FromUrl(data.value());
  EXPECT_TRUE(origin.IsOpaque());
}

} // namespace
} // namespace neko::security
