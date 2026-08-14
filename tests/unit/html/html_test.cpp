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

// WHATWG 13.2.6 "button scope": the "close a p element" step (run by a
// break-out block start tag such as <div>) only closes an open <p> that is not
// shielded by a <button>.  Here the <p> is still open when <div> starts, but a
// <button> sits between it and the top of the stack, so per spec the <p> must
// NOT be closed: <div> becomes a child of the <button> (still inside the <p>).
TEST(HtmlTest, ButtonScopeShieldsPFromBreakoutDiv) {
  auto doc = ParseDoc("<p>a<button>b<div>c");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  // Only one <p> was created; it was not split by the break-out <div>.
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "p").size(), 1u);
  // The <p> still contains the <button>, which contains the <div>.  Were the
  // <p> wrongly closed (default scope), <div> would be a sibling of <button>
  // under <body> instead of a descendant of the <button>.
  dom::Element* button = dom::QuerySelector(*p, "button");
  ASSERT_NE(button, nullptr);
  dom::Element* div = dom::QuerySelector(*button, "div");
  ASSERT_NE(div, nullptr);
  EXPECT_EQ(div->TextContent(), "c");
  EXPECT_EQ(p->TextContent(), "abc");
}

// <ol>/<ul>/<li> are NOT button-scope boundaries, so an open <p> inside one is
// still closed by a break-out <div> (the <p> is NOT shielded).  The <div>
// becomes a sibling of the <p> inside that list item.
TEST(HtmlTest, ListItemScopeDoesNotShieldP) {
  auto doc = ParseDoc("<li><p>a<div>b");
  const auto ps = dom::QuerySelectorAll(*doc, "p");
  ASSERT_EQ(ps.size(), 1u);
  EXPECT_EQ(ps[0]->TextContent(), "a");
  dom::Element* li = dom::QuerySelector(*doc, "li");
  ASSERT_NE(li, nullptr);
  // <div> is a sibling of <p>, both directly inside <li>.
  EXPECT_EQ(li->child_count(), 2u);
  EXPECT_EQ(li->first_child(), static_cast<dom::Node*>(ps[0]));
  dom::Element* div = dom::QuerySelector(*li, "div");
  ASSERT_NE(div, nullptr);
  EXPECT_EQ(div->TextContent(), "b");
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

TEST(HtmlTest, NamedReferenceDoesNotSwallowFollowingText) {
  // A named reference that is a prefix of a longer alnum run must be emitted
  // literally, keeping the trailing characters (WHATWG named character
  // reference state).
  auto doc = ParseDoc("<p>&ampfoo; &ltx &notin; &apos;</p>");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  // &ampfoo; -> literal "&ampfoo;"; &ltx -> literal "&ltx";
  // &notin; -> literal "&notin;" (notin is not in the table);
  // &apos; -> single quote.
  EXPECT_EQ(p->TextContent(), "&ampfoo; &ltx &notin; '");
}

TEST(HtmlTest, RcdataRawtextEndTagClosesWithWhitespace) {
  // A RCDATA/RAWTEXT end tag may be followed by whitespace or a slash before
  // '>' (attributes / self-closing); the element must still close instead of
  // swallowing the rest of the document as text.
  auto doc = ParseDoc("<textarea>a</textarea ><p>x</p>");
  dom::Element* ta = dom::QuerySelector(*doc, "textarea");
  ASSERT_NE(ta, nullptr);
  EXPECT_EQ(ta->TextContent(), "a");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "x");

  auto doc2 = ParseDoc("<style>a</style\n><p>x</p>");
  dom::Element* style = dom::QuerySelector(*doc2, "style");
  ASSERT_NE(style, nullptr);
  EXPECT_EQ(style->TextContent(), "a");
  dom::Element* p2 = dom::QuerySelector(*doc2, "p");
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(p2->TextContent(), "x");
}

TEST(HtmlTest, BeforeHeadEndTagDoesNotPopHtmlRoot) {
  // In the "before head" insertion mode an end tag is a parse error and must
  // be ignored; popping the html root here would make body a sibling of html.
  auto doc = ParseDoc("<html></head><body>x</body></html>");
  dom::Element* html = doc->document_element();
  ASSERT_NE(html, nullptr);
  EXPECT_EQ(html->tag_name(), "html");
  EXPECT_EQ(html->child_count(), 2u);  // head + body
  dom::Element* head = dom::QuerySelector(*doc, "head");
  ASSERT_NE(head, nullptr);
  EXPECT_EQ(head->parent(), html);
  dom::Element* body = dom::QuerySelector(*doc, "body");
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(body->parent(), html);
  EXPECT_EQ(body->TextContent(), "x");
}

TEST(HtmlTest, SelfClosingSlashDoesNotCreateEmptyAttribute) {
  // `<div />` must not produce an empty-named attribute (the '/' routes to
  // the after-attribute-name state, not a new attribute).
  auto doc = ParseDoc("<div />x");
  dom::Element* div = dom::QuerySelector(*doc, "div");
  ASSERT_NE(div, nullptr);
  EXPECT_EQ(div->attributes().size(), 0u);

  // Attributes before the slash are preserved.
  auto doc2 = ParseDoc("<div a=\"b\"/>x");
  dom::Element* div2 = dom::QuerySelector(*doc2, "div");
  ASSERT_NE(div2, nullptr);
  ASSERT_EQ(div2->attributes().size(), 1u);
  EXPECT_EQ(div2->attributes()[0].name, "a");
  EXPECT_EQ(div2->attributes()[0].value, "b");
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

// Regression: the adoption agency inner loop used to re-index the current node
// after removing it from the stack, which returned -1 and dereferenced
// stack_[-2] (overflow) -- for inputs with a non-formatting element (e.g.
// <span>) between the formatting element and the furthest block this looped
// forever.  These all parsed to a well-formed tree instead of hanging.
TEST(HtmlTest, AdoptionAgencySpanBetweenFormattingAndBlock) {
  auto doc = ParseDoc("<b><span>x<div>y</b>");
  EXPECT_EQ(doc->ToString(),
            "<html><head></head><body><b><span>x</span></b><div><b>y</b></div></body></html>");
}

TEST(HtmlTest, AdoptionAgencyEmSpanDiv) {
  auto doc = ParseDoc("<em><span>a<div>b</em>");
  EXPECT_EQ(doc->ToString(),
            "<html><head></head><body><em><span>a</span></em><div><em>b</em></div></body></html>");
}

TEST(HtmlTest, AdoptionAgencyBoldDivParagraph) {
  auto doc = ParseDoc("<b><div><p>x</b>");
  EXPECT_EQ(doc->ToString(),
            "<html><head></head><body><b></b><div><b><p>x</p></b></div></body></html>");
}

TEST(HtmlTest, NoahsArkBoundsFormattingElements) {
  // Four nested <b> elements (same tag + attributes) trigger the Noah's Ark
  // clause; the DOM must still nest correctly.
  auto doc = ParseDoc("<b class=\"x\"><b><b class=\"x\"><b>y</b></b></b></b>");
  EXPECT_EQ(doc->ToString(),
            "<html><head></head><body><b class=\"x\"><b><b class=\"x\"><b>y</b></b></b></b></body></html>");
}

TEST(HtmlTest, ScriptDataEscapedComment) {
  auto doc = ParseDoc("<script>a<!--b-->c</script><p>x</p>");
  dom::Element* script = dom::QuerySelector(*doc, "script");
  ASSERT_NE(script, nullptr);
  EXPECT_EQ(script->TextContent(), "a<!--b-->c");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "x");
}

TEST(HtmlTest, ScriptDataDoubleEscape) {
  // A nested <script> inside an HTML-comment-like <!-- --> region must not
  // close the outer script; it enters the double-escaped state instead.
  auto doc = ParseDoc("<script><!--<script></script>--></script><p>x</p>");
  dom::Element* script = dom::QuerySelector(*doc, "script");
  ASSERT_NE(script, nullptr);
  EXPECT_EQ(script->TextContent(), "<!--<script></script>-->");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "x");
}

// Returns the nesting depth of the deepest element under |node|.
int MaxDepth(const dom::Node* node) {
  int deepest = 1;
  for (dom::Node* child : node->ChildNodes()) {
    if (child->node_type() == dom::NodeType::kElement) {
      deepest = std::max(deepest, 1 + MaxDepth(child));
    }
  }
  return deepest;
}

TEST(HtmlTest, ShallowNestingIsPreserved) {
  // Normal pages well under the depth cap must be kept intact.  The count
  // includes the implied html/body skeleton, so it exceeds 100.
  std::string html;
  for (int i = 0; i < 100; ++i) html += "<div>";
  for (int i = 0; i < 100; ++i) html += "</div>";
  auto doc = ParseDoc(html);
  EXPECT_GE(MaxDepth(doc.get()), 100);
}

TEST(HtmlTest, OverDeepNestingIsCapped) {
  // Pathological nesting must not produce a DOM deep enough to overflow the
  // stack in the recursive style/layout walks; the parser drops the
  // over-deep subtree.
  std::string html;
  for (int i = 0; i < 10000; ++i) html += "<div>";
  for (int i = 0; i < 10000; ++i) html += "</div>";
  auto doc = ParseDoc(html);
  // The depth cap is internal; assert the result stays far below the input
  // nesting so the recursive downstream walks cannot overflow.
  EXPECT_LT(MaxDepth(doc.get()), 1000);
}

}  // namespace
}  // namespace neko::html
