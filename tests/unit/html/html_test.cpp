#include "neko/dom/query.h"
#include "neko/html/parser.h"

#include <memory>
#include <string>

#include <gtest/gtest.h>

namespace neko::html {
namespace {

std::unique_ptr<dom::Document> ParseDoc(std::string_view html) {
  Parser parser(html);
  return parser.Parse();
}

dom::Element* Body(dom::Document& doc) { return dom::QuerySelector(doc, "body"); }

TEST(HtmlTest, FullDocumentStructure) {
  auto doc = ParseDoc(
      "<!DOCTYPE html><html><head><title>Hi</title></head>"
      "<body><p>Hello</p></body></html>");
  dom::Element* html = doc->document_element();
  ASSERT_NE(html, nullptr);
  EXPECT_EQ(html->tag_name(), "html");
  EXPECT_EQ(doc->Title(), "Hi");

  dom::Element* body = Body(*doc);
  ASSERT_NE(body, nullptr);
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "Hello");
}

TEST(HtmlTest, ImpliedHtmlHeadBody) {
  auto doc = ParseDoc("hello");
  dom::Element* html = doc->document_element();
  ASSERT_NE(html, nullptr);
  EXPECT_EQ(html->tag_name(), "html");
  EXPECT_EQ(html->child_count(), 2u);  // head + body
  dom::Element* body = Body(*doc);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(body->TextContent(), "hello");
}

TEST(HtmlTest, ImpliedPEndTag) {
  auto doc = ParseDoc("<p>a<p>b");
  const auto ps = dom::QuerySelectorAll(*doc, "p");
  ASSERT_EQ(ps.size(), 2u);
  EXPECT_EQ(ps[0]->TextContent(), "a");
  EXPECT_EQ(ps[1]->TextContent(), "b");
}

TEST(HtmlTest, VoidElements) {
  auto doc = ParseDoc("<div>a<br><img src=\"x.png\">b</div>");
  dom::Element* div = dom::QuerySelector(*doc, "div");
  ASSERT_NE(div, nullptr);
  EXPECT_EQ(div->child_count(), 4u);
  dom::Element* br = dom::QuerySelector(*doc, "br");
  ASSERT_NE(br, nullptr);
  EXPECT_EQ(br->child_count(), 0u);
  dom::Element* img = dom::QuerySelector(*doc, "img");
  ASSERT_NE(img, nullptr);
  EXPECT_EQ(img->GetAttribute("src").value(), "x.png");
}

TEST(HtmlTest, RawTextScript) {
  auto doc = ParseDoc("<script>var a = \"</p>\"; if (a < b) {}</script><p>after</p>");
  dom::Element* script = dom::QuerySelector(*doc, "script");
  ASSERT_NE(script, nullptr);
  EXPECT_EQ(script->TextContent(), "var a = \"</p>\"; if (a < b) {}");
  // No <p> was created inside the script.
  EXPECT_EQ(dom::QuerySelectorAll(*script, "p").size(), 0u);
  // The closing </script> was consumed correctly.
  dom::Element* p = dom::QuerySelector(*doc, "body p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "after");
}

TEST(HtmlTest, RawTextStyle) {
  auto doc = ParseDoc("<style>p { color: red; }</style><p>x</p>");
  dom::Element* style = dom::QuerySelector(*doc, "style");
  ASSERT_NE(style, nullptr);
  EXPECT_EQ(style->TextContent(), "p { color: red; }");
}

TEST(HtmlTest, CharacterReferences) {
  auto doc = ParseDoc("<p>&amp; &lt; &gt; &copy; &#65; &#x42; &nbsp;x</p>");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "& < > \xC2\xA9 A B \xC2\xA0x");
}

TEST(HtmlTest, Comments) {
  auto doc = ParseDoc("<!-- hello --><p>x</p><!-- done -->");
  // A comment before <html> is attached to the Document.
  EXPECT_EQ(doc->child_count(), 2u);  // comment + html
  EXPECT_EQ(doc->first_child()->node_type(), dom::NodeType::kComment);
  EXPECT_EQ(static_cast<dom::Comment*>(doc->first_child())->data(), " hello ");
  // A trailing comment inside <body> is attached to the body.
  dom::Element* body = Body(*doc);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(body->last_child()->node_type(), dom::NodeType::kComment);
}

TEST(HtmlTest, AttributesQuotingStyles) {
  auto doc = ParseDoc("<div id=\"double\" class='single' data-z=unquoted></div>");
  dom::Element* div = dom::QuerySelector(*doc, "div");
  ASSERT_NE(div, nullptr);
  EXPECT_EQ(div->GetAttribute("id").value(), "double");
  EXPECT_EQ(div->GetAttribute("class").value(), "single");
  EXPECT_EQ(div->GetAttribute("data-z").value(), "unquoted");
}

TEST(HtmlTest, UnorderedListItems) {
  auto doc = ParseDoc("<ul><li>one<li>two</ul>");
  const auto items = dom::QuerySelectorAll(*doc, "li");
  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(items[0]->TextContent(), "one");
  EXPECT_EQ(items[1]->TextContent(), "two");
}

TEST(HtmlTest, HeadingsCloseEachOther) {
  auto doc = ParseDoc("<h1>a<h2>b<h3>c");
  const auto h1 = dom::QuerySelectorAll(*doc, "h1");
  const auto h2 = dom::QuerySelectorAll(*doc, "h2");
  const auto h3 = dom::QuerySelectorAll(*doc, "h3");
  EXPECT_EQ(h1.size(), 1u);
  EXPECT_EQ(h2.size(), 1u);
  EXPECT_EQ(h3.size(), 1u);
}

TEST(HtmlTest, MalformedInput) {
  auto doc = ParseDoc("<div><span>unclosed<div>other</div></span>");
  dom::Element* div = dom::QuerySelector(*doc, "div");
  ASSERT_NE(div, nullptr);
  EXPECT_NE(dom::QuerySelector(*doc, "span"), nullptr);
  EXPECT_NE(dom::QuerySelector(*doc, "div div"), nullptr);
}

TEST(HtmlTest, StrayEndTagIgnored) {
  auto doc = ParseDoc("</div><p>ok</p>");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "ok");
}

TEST(HtmlTest, NestedInlineElements) {
  auto doc = ParseDoc("<p>a <em>b <strong>c</strong> d</em> e</p>");
  dom::Element* em = dom::QuerySelector(*doc, "em");
  ASSERT_NE(em, nullptr);
  dom::Element* strong = dom::QuerySelector(*doc, "strong");
  ASSERT_NE(strong, nullptr);
  EXPECT_EQ(strong->TextContent(), "c");
  EXPECT_EQ(dom::QuerySelector(*doc, "p")->TextContent(), "a b c d e");
}

TEST(HtmlTest, EmptyDocument) {
  auto doc = ParseDoc("");
  EXPECT_NE(doc->document_element(), nullptr);
  EXPECT_EQ(doc->document_element()->tag_name(), "html");
}

TEST(HtmlTest, WhitespacePreserved) {
  auto doc = ParseDoc("<p>  spaced  </p>");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "  spaced  ");
}

TEST(HtmlTest, AdoptionAgencyReconstructsFormatting) {
  auto doc = ParseDoc("<b>1<i>2</b>3</i>");
  EXPECT_EQ(doc->ToString(),
            "<html><head></head><body><b>1<i>2</i></b><i>3</i></body></html>");
}

TEST(HtmlTest, AdoptionAgencyCanonical) {
  auto doc = ParseDoc("<p>1<b>2<i>3</b>4</i>5</p>");
  EXPECT_EQ(doc->ToString(),
            "<html><head></head><body><p>1<b>2<i>3</i></b><i>4</i>5</p></body></html>");
}

TEST(HtmlTest, AdoptionAgencyWithFurthestBlock) {
  // WHATWG 13.2.10.2: a special element (p) inside the formatting element.
  auto doc = ParseDoc("<b>1<p>2</b>3</p>");
  EXPECT_EQ(doc->ToString(),
            "<html><head></head><body><b>1</b><p><b>2</b>3</p></body></html>");
}

}  // namespace
}  // namespace neko::html
