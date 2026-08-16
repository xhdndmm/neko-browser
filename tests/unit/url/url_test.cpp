#include "neko/url/url.h"

#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace neko::url {
namespace {

TEST(UrlTest, ParseSimpleHttp)
{
  const auto r = Url::Parse("http://example.com");
  ASSERT_TRUE(r.has_value());
  const Url& u = r.value();
  EXPECT_EQ(u.scheme(), "http");
  EXPECT_EQ(u.host(), "example.com");
  EXPECT_FALSE(u.port().has_value());
  EXPECT_EQ(u.path(), "/");
  EXPECT_EQ(u.effective_port(), 80);
  EXPECT_EQ(u.Serialize(), "http://example.com/");
  EXPECT_EQ(u.Origin(), "http://example.com");
}

TEST(UrlTest, ParseHttpsWithPath)
{
  const auto r = Url::Parse("https://example.com/a/b");
  ASSERT_TRUE(r.has_value());
  const Url& u = r.value();
  EXPECT_EQ(u.scheme(), "https");
  EXPECT_EQ(u.path(), "/a/b");
  EXPECT_EQ(u.effective_port(), 443);
}

TEST(UrlTest, ParsePort)
{
  const auto r = Url::Parse("https://example.com:8443/path");
  ASSERT_TRUE(r.has_value());
  const Url& u = r.value();
  ASSERT_TRUE(u.port().has_value());
  EXPECT_EQ(u.port().value(), 8443);
  EXPECT_EQ(u.Serialize(), "https://example.com:8443/path");
  EXPECT_EQ(u.Origin(), "https://example.com:8443");
}

TEST(UrlTest, ParseQueryAndFragment)
{
  const auto r = Url::Parse("https://example.com/a/b?x=1#section");
  ASSERT_TRUE(r.has_value());
  const Url& u = r.value();
  EXPECT_EQ(u.path(), "/a/b");
  ASSERT_TRUE(u.has_query());
  EXPECT_EQ(u.query(), "x=1");
  ASSERT_TRUE(u.has_fragment());
  EXPECT_EQ(u.fragment(), "section");
  EXPECT_EQ(u.Serialize(), "https://example.com/a/b?x=1");
  EXPECT_EQ(u.Serialize(/*include_fragment=*/true), "https://example.com/a/b?x=1#section");
}

TEST(UrlTest, ParseUserInfo)
{
  const auto r = Url::Parse("https://user:password@example.com/");
  ASSERT_TRUE(r.has_value());
  const Url& u = r.value();
  EXPECT_EQ(u.username(), "user");
  EXPECT_EQ(u.password(), "password");
  EXPECT_EQ(u.host(), "example.com");
  EXPECT_EQ(u.Serialize(), "https://user:password@example.com/");
}

TEST(UrlTest, SchemeAndHostLowercased)
{
  const auto r = Url::Parse("HTTP://EXAMPLE.COM/Path");
  ASSERT_TRUE(r.has_value());
  const Url& u = r.value();
  EXPECT_EQ(u.scheme(), "http");
  EXPECT_EQ(u.host(), "example.com");
  // Path case is preserved.
  EXPECT_EQ(u.path(), "/Path");
}

TEST(UrlTest, DefaultPortOmittedInSerialization)
{
  const auto r = Url::Parse("http://example.com:80/");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value().Serialize(), "http://example.com/");
}

TEST(UrlTest, DotSegmentsRemoved)
{
  const auto r = Url::Parse("http://example.com/a/b/../c/./d");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value().path(), "/a/c/d");
}

TEST(UrlTest, TrailingDotDot)
{
  const auto r = Url::Parse("http://example.com/a/b/..");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value().path(), "/a/");
}

TEST(UrlTest, RelativePathResolution)
{
  const auto base = Url::Parse("http://a.example/dir/page.html");
  ASSERT_TRUE(base.has_value());
  const Url& b = base.value();

  const auto r1 = Url::Parse("other.html", b);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value().Serialize(), "http://a.example/dir/other.html");

  const auto r2 = Url::Parse("/top.html", b);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value().Serialize(), "http://a.example/top.html");

  const auto r3 = Url::Parse("../up.html", b);
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3.value().Serialize(), "http://a.example/up.html");

  const auto r4 = Url::Parse("?q=2", b);
  ASSERT_TRUE(r4.has_value());
  EXPECT_EQ(r4.value().Serialize(), "http://a.example/dir/page.html?q=2");

  const auto r5 = Url::Parse("#frag", b);
  ASSERT_TRUE(r5.has_value());
  EXPECT_EQ(r5.value().Serialize(true), "http://a.example/dir/page.html#frag");

  const auto r6 = Url::Parse("", b);
  ASSERT_TRUE(r6.has_value());
  EXPECT_EQ(r6.value().Serialize(true), "http://a.example/dir/page.html");
}

TEST(UrlTest, NetworkPathResolution)
{
  const auto base = Url::Parse("https://a.example/dir/page.html");
  ASSERT_TRUE(base.has_value());
  const auto r = Url::Parse("//b.example/root", base.value());
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value().Serialize(), "https://b.example/root");
}

TEST(UrlTest, AbsoluteBeatsBase)
{
  const auto base = Url::Parse("http://a.example/x");
  ASSERT_TRUE(base.has_value());
  const auto r = Url::Parse("https://b.example/y", base.value());
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value().scheme(), "https");
  EXPECT_EQ(r.value().host(), "b.example");
}

TEST(UrlTest, Rfc3986ResolutionExamples)
{
  // RFC 3986 5.4.1 Normal Examples
  const auto base = Url::Parse("http://a/b/c/d;p?q");
  ASSERT_TRUE(base.has_value());
  const Url& b = base.value();

  struct Case
  {
    std::string_view ref;
    std::string_view expected;
  };
  constexpr Case kCases[] = {
      {"g", "http://a/b/c/g"},
      {"./g", "http://a/b/c/g"},
      {"g/", "http://a/b/c/g/"},
      {"/g", "http://a/g"},
      // WHATWG serializes empty paths with a trailing '/' for special
      // schemes (RFC 3986 prints "http://g" without it).
      {"//g", "http://g/"},
      {"?y", "http://a/b/c/d;p?y"},
      {"g?y", "http://a/b/c/g?y"},
      {"#s", "http://a/b/c/d;p?q#s"},
      {"g#s", "http://a/b/c/g#s"},
      {"g?y#s", "http://a/b/c/g?y#s"},
      {";x", "http://a/b/c/;x"},
      {"g;x", "http://a/b/c/g;x"},
      {"g;x?y#s", "http://a/b/c/g;x?y#s"},
      {"", "http://a/b/c/d;p?q"},
      {".", "http://a/b/c/"},
      {"./", "http://a/b/c/"},
      {"..", "http://a/b/"},
      {"../", "http://a/b/"},
      {"../g", "http://a/b/g"},
      {"../..", "http://a/"},
      {"../../", "http://a/"},
      {"../../g", "http://a/g"},
  };
  for (const Case& c : kCases) {
    const auto r = Url::Parse(c.ref, b);
    ASSERT_TRUE(r.has_value()) << "reference: " << c.ref;
    EXPECT_EQ(r.value().Serialize(true), c.expected) << "reference: " << c.ref;
  }
}

TEST(UrlTest, InvalidUrls)
{
  EXPECT_FALSE(Url::Parse("").has_value());
  EXPECT_FALSE(Url::Parse("example.com").has_value());              // no scheme
  EXPECT_FALSE(Url::Parse("1http://x.com").has_value());            // scheme must start with letter
  EXPECT_FALSE(Url::Parse("http://").has_value());                  // empty host
  EXPECT_FALSE(Url::Parse("http://example.com:abc").has_value());   // bad port
  EXPECT_FALSE(Url::Parse("http://example.com:99999").has_value()); // port overflow
  EXPECT_FALSE(Url::Parse("http://exa mple.com/").has_value());     // space in host
  EXPECT_FALSE(Url::Parse("http:/example.com").has_value());        // special scheme needs //
  EXPECT_FALSE(Url::Parse("http://[::1").has_value());              // unterminated IPv6
  // Control characters in path/query would let untrusted input inject bytes
  // into the HTTP request line; they must be rejected.
  EXPECT_FALSE(Url::Parse("http://example.com/\r\nX-Injected: 1").has_value());
  EXPECT_FALSE(Url::Parse("http://example.com/\n").has_value());
  EXPECT_FALSE(Url::Parse("http://example.com/?a=b\r\nc").has_value());
  // Relative resolution must reject control characters too.
  const auto base = Url::Parse("http://example.com/base/");
  ASSERT_TRUE(base.has_value());
  EXPECT_FALSE(Url::Parse("\r\n", base.value()).has_value());
  EXPECT_FALSE(Url::Parse("x\r\n", base.value()).has_value());
}

TEST(UrlTest, IPv6Host)
{
  const auto r = Url::Parse("http://[::1]:8080/x");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value().host(), "::1");
  ASSERT_TRUE(r.value().port().has_value());
  EXPECT_EQ(r.value().port().value(), 8080);
  EXPECT_EQ(r.value().Serialize(), "http://[::1]:8080/x");
}

TEST(UrlTest, NonSpecialScheme)
{
  const auto r = Url::Parse("mailto:user@example.com");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value().scheme(), "mailto");
  EXPECT_FALSE(r.value().has_authority());
  EXPECT_EQ(r.value().path(), "user@example.com");
}

TEST(UrlTest, PercentEncodeDecode)
{
  EXPECT_EQ(PercentEncode("a b/c"), "a%20b%2Fc");
  EXPECT_EQ(PercentEncode("AZaz09-._~"), "AZaz09-._~");
  EXPECT_EQ(PercentDecode("a%20b%2Fc"), "a b/c");
  EXPECT_EQ(PercentDecode("100%"), "100%"); // lone % kept
  EXPECT_EQ(PercentDecode("x%zz"), "x%zz"); // invalid hex kept
  EXPECT_EQ(PercentDecode(PercentEncode("héllo/世界")), "héllo/世界");
}

TEST(UrlTest, EffectivePort)
{
  EXPECT_EQ(DefaultPortForScheme("http"), 80);
  EXPECT_EQ(DefaultPortForScheme("https"), 443);
  EXPECT_EQ(DefaultPortForScheme("ftp"), 21);
  EXPECT_EQ(DefaultPortForScheme("ws"), 80);
  EXPECT_EQ(DefaultPortForScheme("wss"), 443);
  EXPECT_EQ(DefaultPortForScheme("custom"), 0);
}

TEST(UrlTest, OriginForDefaultAndCustomPort)
{
  const auto a = Url::Parse("http://example.com/path");
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a.value().Origin(), "http://example.com");

  const auto b = Url::Parse("http://example.com:8080/path");
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b.value().Origin(), "http://example.com:8080");
}

} // namespace
} // namespace neko::url
