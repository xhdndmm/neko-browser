// Unit tests for hyperlink resolution (neko::browser::HyperlinkTarget).

#include "neko/browser/hyperlink.h"
#include "neko/dom/query.h"
#include "neko/html/parser.h"

#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>

namespace neko::browser {
namespace {

std::unique_ptr<dom::Document> Parse(std::string_view html)
{
  return html::Parser(html).Parse();
}

// The browser routes these inputs to the filesystem on Windows: a drive path
// is also a valid URL there ("C:\dir" parses as the one-letter scheme "c"), so
// the classification has to happen before the URL parse.
TEST(WindowsLocalPathTest, DetectsDriveAndUncPaths)
{
  EXPECT_TRUE(IsWindowsLocalPath("C:\\dir\\page.html"));
  EXPECT_TRUE(IsWindowsLocalPath("c:/dir/page.html"));
  EXPECT_TRUE(IsWindowsLocalPath("C:"));
  EXPECT_TRUE(IsWindowsLocalPath("C:page.html"));
  EXPECT_TRUE(IsWindowsLocalPath("\\\\server\\share\\page.html"));
}

TEST(WindowsLocalPathTest, LeavesUrlsAlone)
{
  EXPECT_FALSE(IsWindowsLocalPath("https://example.com/page"));
  EXPECT_FALSE(IsWindowsLocalPath("file:///tmp/page.html"));
  EXPECT_FALSE(IsWindowsLocalPath("data:text/plain,hello"));
  EXPECT_FALSE(IsWindowsLocalPath("/tmp/page.html"));
  EXPECT_FALSE(IsWindowsLocalPath("relative/page.html"));
  EXPECT_FALSE(IsWindowsLocalPath(""));
  EXPECT_FALSE(IsWindowsLocalPath("example.com"));
}

// Relative references on a local-file page resolve by string concatenation.
// This must hold for Windows drive bases too: "C:\dir\page.html" parses as the
// one-letter scheme "c", and a URL round trip would lowercase the drive letter
// (the regression behind a local form submit's tab URL: "c:" vs "C:").
TEST(ResolveReferenceTest, WindowsDriveBaseKeepsThePathVerbatim)
{
  const std::optional<std::string> resolved = ResolveReference("form.html", "C:\\dir\\page.html");
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(*resolved, "C:\\dir\\form.html");

  // Mixed separators (a CMake-style source dir joined to a '/'-suffixed
  // fixture name) survive as they are.
  const std::optional<std::string> mixed =
      ResolveReference("form.html", "C:\\Users\\RUNNER~1\\Temp/t.html");
  ASSERT_TRUE(mixed.has_value());
  EXPECT_EQ(*mixed, "C:\\Users\\RUNNER~1\\Temp/form.html");
}

TEST(ResolveReferenceTest, WindowsRootRelativeStaysOnTheSameDrive)
{
  const std::optional<std::string> resolved = ResolveReference("/root.html", "C:\\dir\\page.html");
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(*resolved, "C:/root.html");
}

TEST(HyperlinkTargetTest, DirectAnchor)
{
  auto doc = Parse("<html><body><a href=\"https://example.com/page\">x</a></body></html>");
  dom::Element* a = dom::QuerySelector(*doc, "a");
  ASSERT_NE(a, nullptr);
  const std::optional<std::string> target = HyperlinkTarget(a, "https://example.com/");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(*target, "https://example.com/page");
}

TEST(HyperlinkTargetTest, DescendantOfAnchor)
{
  // Clicking any descendant of an <a> activates that hyperlink (WHATWG HTML
  // §4.6.5): the <b> is not itself an anchor but resolves to its ancestor.
  auto doc = Parse("<html><body><a href=\"next\"><b>click me</b></a></body></html>");
  dom::Element* b = dom::QuerySelector(*doc, "b");
  ASSERT_NE(b, nullptr);
  const std::optional<std::string> target =
      HyperlinkTarget(b, "https://example.com/dir/index.html");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(*target, "https://example.com/dir/next");
}

TEST(HyperlinkTargetTest, RelativePathWithDotSegments)
{
  auto doc = Parse("<html><body><a href=\"../up\">x</a></body></html>");
  dom::Element* a = dom::QuerySelector(*doc, "a");
  ASSERT_NE(a, nullptr);
  const std::optional<std::string> target = HyperlinkTarget(a, "https://example.com/a/b/c.html");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(*target, "https://example.com/a/up");
}

TEST(HyperlinkTargetTest, FragmentOnlyReference)
{
  auto doc = Parse("<html><body><a href=\"#section\">x</a></body></html>");
  dom::Element* a = dom::QuerySelector(*doc, "a");
  ASSERT_NE(a, nullptr);
  const std::optional<std::string> target = HyperlinkTarget(a, "https://example.com/page");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(*target, "https://example.com/page#section");
}

TEST(HyperlinkTargetTest, NotInsideAnchor)
{
  auto doc = Parse("<html><body><p>plain text</p></body></html>");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_FALSE(HyperlinkTarget(p, "https://example.com/").has_value());
  EXPECT_FALSE(HyperlinkTarget(nullptr, "https://example.com/").has_value());
}

TEST(HyperlinkTargetTest, AnchorWithoutHrefIsNotHyperlink)
{
  auto doc = Parse("<html><body><a>name only</a></body></html>");
  dom::Element* a = dom::QuerySelector(*doc, "a");
  ASSERT_NE(a, nullptr);
  EXPECT_FALSE(HyperlinkTarget(a, "https://example.com/").has_value());
}

} // namespace
} // namespace neko::browser
