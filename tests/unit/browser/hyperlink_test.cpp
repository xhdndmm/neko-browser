// Unit tests for hyperlink resolution (neko::browser::HyperlinkTarget).

#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "neko/browser/hyperlink.h"
#include "neko/dom/query.h"
#include "neko/html/parser.h"

namespace neko::browser {
namespace {

std::unique_ptr<dom::Document> Parse(std::string_view html) {
  return html::Parser(html).Parse();
}

TEST(HyperlinkTargetTest, DirectAnchor) {
  auto doc = Parse("<html><body><a href=\"https://example.com/page\">x</a></body></html>");
  dom::Element* a = dom::QuerySelector(*doc, "a");
  ASSERT_NE(a, nullptr);
  const std::optional<std::string> target = HyperlinkTarget(a, "https://example.com/");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(*target, "https://example.com/page");
}

TEST(HyperlinkTargetTest, DescendantOfAnchor) {
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

TEST(HyperlinkTargetTest, RelativePathWithDotSegments) {
  auto doc = Parse("<html><body><a href=\"../up\">x</a></body></html>");
  dom::Element* a = dom::QuerySelector(*doc, "a");
  ASSERT_NE(a, nullptr);
  const std::optional<std::string> target =
      HyperlinkTarget(a, "https://example.com/a/b/c.html");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(*target, "https://example.com/a/up");
}

TEST(HyperlinkTargetTest, FragmentOnlyReference) {
  auto doc = Parse("<html><body><a href=\"#section\">x</a></body></html>");
  dom::Element* a = dom::QuerySelector(*doc, "a");
  ASSERT_NE(a, nullptr);
  const std::optional<std::string> target =
      HyperlinkTarget(a, "https://example.com/page");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(*target, "https://example.com/page#section");
}

TEST(HyperlinkTargetTest, NotInsideAnchor) {
  auto doc = Parse("<html><body><p>plain text</p></body></html>");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_FALSE(HyperlinkTarget(p, "https://example.com/").has_value());
  EXPECT_FALSE(HyperlinkTarget(nullptr, "https://example.com/").has_value());
}

TEST(HyperlinkTargetTest, AnchorWithoutHrefIsNotHyperlink) {
  auto doc = Parse("<html><body><a>name only</a></body></html>");
  dom::Element* a = dom::QuerySelector(*doc, "a");
  ASSERT_NE(a, nullptr);
  EXPECT_FALSE(HyperlinkTarget(a, "https://example.com/").has_value());
}

}  // namespace
}  // namespace neko::browser
