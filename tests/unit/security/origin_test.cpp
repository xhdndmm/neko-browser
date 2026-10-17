// Unit tests for neko::security::Origin (the Same-Origin Policy basis).

#include "neko/security/origin.h"
#include "neko/security/public_suffix.h"
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

// ---------------------------------------------------------------------------
// Public Suffix List (tools/gen_psl.py -> public_suffix_data.inc).
// ---------------------------------------------------------------------------

TEST(PublicSuffixTest, IcannRules)
{
  EXPECT_EQ(RegistrableDomain("www.example.com"), "example.com");
  EXPECT_EQ(RegistrableDomain("example.com"), "example.com");
  EXPECT_EQ(RegistrableDomain("deep.sub.example.co.uk"), "example.co.uk");
  EXPECT_EQ(PublicSuffix("example.co.uk"), "co.uk");
  EXPECT_EQ(PublicSuffix("com"), "com");
  // The default "*" rule: an unknown TLD's suffix is its rightmost label.
  EXPECT_EQ(PublicSuffix("example.unknown-tld"), "unknown-tld");
  EXPECT_EQ(RegistrableDomain("example.unknown-tld"), "example.unknown-tld");
}

TEST(PublicSuffixTest, WildcardAndExceptionRules)
{
  // *.ck plus !www.ck (the classic pair in the list).
  EXPECT_EQ(PublicSuffix("anything.ck"), "anything.ck");
  EXPECT_EQ(RegistrableDomain("anything.ck"), "");
  EXPECT_EQ(RegistrableDomain("y.x.ck"), "y.x.ck");
  // The exception: www.ck is NOT a public suffix; its suffix is ck.
  EXPECT_EQ(PublicSuffix("www.ck"), "ck");
  EXPECT_EQ(RegistrableDomain("www.ck"), "www.ck");
  EXPECT_FALSE(IsPublicSuffix("www.ck"));
}

TEST(PublicSuffixTest, PrivateRulesScopeCookies)
{
  // blogspot.com is a PRIVATE-section rule: it is a public suffix for
  // scoping purposes, so foo.blogspot.com is a registrable domain and
  // blogspot.com itself must never own cookies.
  EXPECT_TRUE(IsPublicSuffix("blogspot.com"));
  EXPECT_EQ(RegistrableDomain("foo.blogspot.com"), "foo.blogspot.com");
  EXPECT_EQ(RegistrableDomain("blogspot.com"), "");
  EXPECT_FALSE(IsPublicSuffix("example.com"));
  EXPECT_TRUE(IsPublicSuffix("co.uk"));
}

TEST(PublicSuffixTest, Ipv4AndIpv6AreNeverPublicSuffixes)
{
  EXPECT_FALSE(IsPublicSuffix("127.0.0.1"));
  EXPECT_EQ(RegistrableDomain("127.0.0.1"), "127.0.0.1");
  EXPECT_FALSE(IsPublicSuffix("::1"));
  EXPECT_EQ(RegistrableDomain("2001:db8::1"), "2001:db8::1");
}

TEST(PublicSuffixTest, Normalization)
{
  // ASCII case folding and one trailing dot.
  EXPECT_EQ(RegistrableDomain("WWW.EXAMPLE.COM."), "example.com");
  EXPECT_TRUE(IsPublicSuffix("CO.UK."));
}

} // namespace
} // namespace neko::security
