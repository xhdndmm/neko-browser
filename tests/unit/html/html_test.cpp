#include "neko/dom/query.h"
#include "neko/html/parser.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>

namespace neko::html {
namespace {

std::unique_ptr<dom::Document> ParseDoc(std::string_view html)
{
  Parser parser(html);
  return parser.Parse();
}

dom::Element* Body(dom::Document& doc)
{
  return dom::QuerySelector(doc, "body");
}

TEST(HtmlTest, FullDocumentStructure)
{
  auto doc = ParseDoc("<!DOCTYPE html><html><head><title>Hi</title></head>"
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

TEST(HtmlTest, ImpliedHtmlHeadBody)
{
  auto doc = ParseDoc("hello");
  dom::Element* html = doc->document_element();
  ASSERT_NE(html, nullptr);
  EXPECT_EQ(html->tag_name(), "html");
  EXPECT_EQ(html->child_count(), 2u); // head + body
  dom::Element* body = Body(*doc);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(body->TextContent(), "hello");
}

TEST(HtmlTest, ImpliedPEndTag)
{
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
TEST(HtmlTest, ButtonScopeShieldsPFromBreakoutDiv)
{
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
TEST(HtmlTest, ListItemScopeDoesNotShieldP)
{
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

TEST(HtmlTest, VoidElements)
{
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

TEST(HtmlTest, RawTextScript)
{
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

TEST(HtmlTest, RawTextStyle)
{
  auto doc = ParseDoc("<style>p { color: red; }</style><p>x</p>");
  dom::Element* style = dom::QuerySelector(*doc, "style");
  ASSERT_NE(style, nullptr);
  EXPECT_EQ(style->TextContent(), "p { color: red; }");
}

TEST(HtmlTest, CharacterReferences)
{
  auto doc = ParseDoc("<p>&amp; &lt; &gt; &copy; &#65; &#x42; &nbsp;x</p>");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "& < > \xC2\xA9 A B \xC2\xA0x");
}

TEST(HtmlTest, NamedReferenceDoesNotSwallowFollowingText)
{
  // A named reference that is a prefix of a longer alnum run must be emitted
  // literally, keeping the trailing characters (WHATWG named character
  // reference state).
  auto doc = ParseDoc("<p>&ampfoo; &ltx &notin; &apos;</p>");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  // &ampfoo; -> literal "&ampfoo;"; &ltx -> literal "&ltx";
  // &notin; -> U+2209 (∉); &apos; -> single quote.
  EXPECT_EQ(p->TextContent(), "&ampfoo; &ltx \xE2\x88\x89 '");
}

TEST(HtmlTest, FullEntityTableLookups)
{
  // Entities beyond the old common subset resolve against the full WHATWG
  // table: &rArr; (U+21D2), &Implies; (U+21D2), &hellip; (U+2026),
  // &NotEqualTilde; is a two-codepoint sequence (U+2242 U+0338).
  auto doc = ParseDoc("<p>&rArr; &Implies; &hellip; &NotEqualTilde;</p>");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "\xE2\x87\x92 \xE2\x87\x92 \xE2\x80\xA6 \xE2\x89\x82\xCC\xB8");
}

TEST(HtmlTest, LegacyNoSemicolonEntities)
{
  // Legacy names resolve without a trailing semicolon when not followed by an
  // alnum; &AElig, &copy and &amp keep the following text.
  auto doc = ParseDoc("<p>&copy x &copyx &AElig &amp;x</p>");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  // &copy x -> ©x; &copyx -> literal (alnum follows); &AElig -> Æ; &amp;x -> &x.
  EXPECT_EQ(p->TextContent(), "\xC2\xA9 x &copyx \xC3\x86 &x");
}

TEST(HtmlTest, RcdataRawtextEndTagClosesWithWhitespace)
{
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

TEST(HtmlTest, BeforeHeadEndTagDoesNotPopHtmlRoot)
{
  // In the "before head" insertion mode an end tag is a parse error and must
  // be ignored; popping the html root here would make body a sibling of html.
  auto doc = ParseDoc("<html></head><body>x</body></html>");
  dom::Element* html = doc->document_element();
  ASSERT_NE(html, nullptr);
  EXPECT_EQ(html->tag_name(), "html");
  EXPECT_EQ(html->child_count(), 2u); // head + body
  dom::Element* head = dom::QuerySelector(*doc, "head");
  ASSERT_NE(head, nullptr);
  EXPECT_EQ(head->parent(), html);
  dom::Element* body = dom::QuerySelector(*doc, "body");
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(body->parent(), html);
  EXPECT_EQ(body->TextContent(), "x");
}

TEST(HtmlTest, SelfClosingSlashDoesNotCreateEmptyAttribute)
{
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

TEST(HtmlTest, Comments)
{
  auto doc = ParseDoc("<!-- hello --><p>x</p><!-- done -->");
  // A comment before <html> is attached to the Document.
  EXPECT_EQ(doc->child_count(), 2u); // comment + html
  EXPECT_EQ(doc->first_child()->node_type(), dom::NodeType::kComment);
  EXPECT_EQ(static_cast<dom::Comment*>(doc->first_child())->data(), " hello ");
  // A trailing comment inside <body> is attached to the body.
  dom::Element* body = Body(*doc);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(body->last_child()->node_type(), dom::NodeType::kComment);
}

TEST(HtmlTest, AttributesQuotingStyles)
{
  auto doc = ParseDoc("<div id=\"double\" class='single' data-z=unquoted></div>");
  dom::Element* div = dom::QuerySelector(*doc, "div");
  ASSERT_NE(div, nullptr);
  EXPECT_EQ(div->GetAttribute("id").value(), "double");
  EXPECT_EQ(div->GetAttribute("class").value(), "single");
  EXPECT_EQ(div->GetAttribute("data-z").value(), "unquoted");
}

TEST(HtmlTest, UnorderedListItems)
{
  auto doc = ParseDoc("<ul><li>one<li>two</ul>");
  const auto items = dom::QuerySelectorAll(*doc, "li");
  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(items[0]->TextContent(), "one");
  EXPECT_EQ(items[1]->TextContent(), "two");
}

// A <li> inside a nested <ul>/<ol> must not close an outer <li> (WHATWG
// 13.2.6.4.7: the li search stops at a special element other than
// address/div/p, and <ul>/<ol> are special).
TEST(HtmlTest, NestedListItemsDoNotCloseOuterLi)
{
  auto doc = ParseDoc("<ul><li>a<ul><li>x</li></ul></li><li>b</li></ul>");
  const auto lis = dom::QuerySelectorAll(*doc, "li");
  ASSERT_EQ(lis.size(), 3u);
  // li[0] contains the nested ul, li[1] is inside it, li[2] is the outer
  // second item.
  EXPECT_EQ(lis[0]->TextContent(), "ax");
  EXPECT_EQ(lis[1]->TextContent(), "x");
  EXPECT_EQ(lis[2]->TextContent(), "b");
  // The nested ul is a child of the first li, not a sibling.
  dom::Element* inner_ul = dom::QuerySelector(*lis[0], "ul");
  ASSERT_NE(inner_ul, nullptr);
  EXPECT_EQ(inner_ul->parent(), lis[0]);
  // Both the inner li and the outer second li share the outer ul ancestor.
  EXPECT_EQ(lis[1]->parent(), inner_ul);
  EXPECT_EQ(lis[2]->parent(), lis[0]->parent());
}

TEST(HtmlTest, HeadingsCloseEachOther)
{
  auto doc = ParseDoc("<h1>a<h2>b<h3>c");
  const auto h1 = dom::QuerySelectorAll(*doc, "h1");
  const auto h2 = dom::QuerySelectorAll(*doc, "h2");
  const auto h3 = dom::QuerySelectorAll(*doc, "h3");
  EXPECT_EQ(h1.size(), 1u);
  EXPECT_EQ(h2.size(), 1u);
  EXPECT_EQ(h3.size(), 1u);
}

TEST(HtmlTest, MalformedInput)
{
  auto doc = ParseDoc("<div><span>unclosed<div>other</div></span>");
  dom::Element* div = dom::QuerySelector(*doc, "div");
  ASSERT_NE(div, nullptr);
  EXPECT_NE(dom::QuerySelector(*doc, "span"), nullptr);
  EXPECT_NE(dom::QuerySelector(*doc, "div div"), nullptr);
}

TEST(HtmlTest, StrayEndTagIgnored)
{
  auto doc = ParseDoc("</div><p>ok</p>");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "ok");
}

TEST(HtmlTest, NestedInlineElements)
{
  auto doc = ParseDoc("<p>a <em>b <strong>c</strong> d</em> e</p>");
  dom::Element* em = dom::QuerySelector(*doc, "em");
  ASSERT_NE(em, nullptr);
  dom::Element* strong = dom::QuerySelector(*doc, "strong");
  ASSERT_NE(strong, nullptr);
  EXPECT_EQ(strong->TextContent(), "c");
  EXPECT_EQ(dom::QuerySelector(*doc, "p")->TextContent(), "a b c d e");
}

TEST(HtmlTest, EmptyDocument)
{
  auto doc = ParseDoc("");
  EXPECT_NE(doc->document_element(), nullptr);
  EXPECT_EQ(doc->document_element()->tag_name(), "html");
}

TEST(HtmlTest, WhitespacePreserved)
{
  auto doc = ParseDoc("<p>  spaced  </p>");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "  spaced  ");
}

TEST(HtmlTest, AdoptionAgencyReconstructsFormatting)
{
  auto doc = ParseDoc("<b>1<i>2</b>3</i>");
  EXPECT_EQ(doc->ToString(), "<html><head></head><body><b>1<i>2</i></b><i>3</i></body></html>");
}

TEST(HtmlTest, AdoptionAgencyCanonical)
{
  auto doc = ParseDoc("<p>1<b>2<i>3</b>4</i>5</p>");
  EXPECT_EQ(doc->ToString(),
            "<html><head></head><body><p>1<b>2<i>3</i></b><i>4</i>5</p></body></html>");
}

TEST(HtmlTest, AdoptionAgencyWithFurthestBlock)
{
  // WHATWG 13.2.10.2: a special element (p) inside the formatting element.
  auto doc = ParseDoc("<b>1<p>2</b>3</p>");
  EXPECT_EQ(doc->ToString(), "<html><head></head><body><b>1</b><p><b>2</b>3</p></body></html>");
}

// Regression: the adoption agency inner loop used to re-index the current node
// after removing it from the stack, which returned -1 and dereferenced
// stack_[-2] (overflow) -- for inputs with a non-formatting element (e.g.
// <span>) between the formatting element and the furthest block this looped
// forever.  These all parsed to a well-formed tree instead of hanging.
TEST(HtmlTest, AdoptionAgencySpanBetweenFormattingAndBlock)
{
  auto doc = ParseDoc("<b><span>x<div>y</b>");
  EXPECT_EQ(doc->ToString(),
            "<html><head></head><body><b><span>x</span></b><div><b>y</b></div></body></html>");
}

TEST(HtmlTest, AdoptionAgencyEmSpanDiv)
{
  auto doc = ParseDoc("<em><span>a<div>b</em>");
  EXPECT_EQ(doc->ToString(),
            "<html><head></head><body><em><span>a</span></em><div><em>b</em></div></body></html>");
}

TEST(HtmlTest, AdoptionAgencyBoldDivParagraph)
{
  auto doc = ParseDoc("<b><div><p>x</b>");
  EXPECT_EQ(doc->ToString(),
            "<html><head></head><body><b></b><div><b><p>x</p></b></div></body></html>");
}

TEST(HtmlTest, NoahsArkBoundsFormattingElements)
{
  // Four nested <b> elements (same tag + attributes) trigger the Noah's Ark
  // clause; the DOM must still nest correctly.
  auto doc = ParseDoc("<b class=\"x\"><b><b class=\"x\"><b>y</b></b></b></b>");
  EXPECT_EQ(doc->ToString(),
            "<html><head></head><body><b class=\"x\"><b><b "
            "class=\"x\"><b>y</b></b></b></b></body></html>");
}

TEST(HtmlTest, ScriptDataEscapedComment)
{
  auto doc = ParseDoc("<script>a<!--b-->c</script><p>x</p>");
  dom::Element* script = dom::QuerySelector(*doc, "script");
  ASSERT_NE(script, nullptr);
  EXPECT_EQ(script->TextContent(), "a<!--b-->c");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "x");
}

TEST(HtmlTest, ScriptDataDoubleEscape)
{
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
int MaxDepth(const dom::Node* node)
{
  int deepest = 1;
  for (dom::Node* child : node->ChildNodes()) {
    if (child->node_type() == dom::NodeType::kElement) {
      deepest = std::max(deepest, 1 + MaxDepth(child));
    }
  }
  return deepest;
}

TEST(HtmlTest, ShallowNestingIsPreserved)
{
  // Normal pages well under the depth cap must be kept intact.  The count
  // includes the implied html/body skeleton, so it exceeds 100.
  std::string html;
  for (int i = 0; i < 100; ++i)
    html += "<div>";
  for (int i = 0; i < 100; ++i)
    html += "</div>";
  auto doc = ParseDoc(html);
  EXPECT_GE(MaxDepth(doc.get()), 100);
}

TEST(HtmlTest, OverDeepNestingIsCapped)
{
  // Pathological nesting must not produce a DOM deep enough to overflow the
  // stack in the recursive style/layout walks; the parser drops the
  // over-deep subtree.
  std::string html;
  for (int i = 0; i < 10000; ++i)
    html += "<div>";
  for (int i = 0; i < 10000; ++i)
    html += "</div>";
  auto doc = ParseDoc(html);
  // The depth cap is internal; assert the result stays far below the input
  // nesting so the recursive downstream walks cannot overflow.
  EXPECT_LT(MaxDepth(doc.get()), 1000);
}

// ---- Newline normalization (13.2.3.5) ------------------------------------

TEST(HtmlTest, CarriageReturnsNormalizedToLf)
{
  // CRLF and bare CR are both normalized to LF before tokenization; newlines
  // in the DOM are always U+000A.
  auto doc = ParseDoc("<p>a\r\nb\rc</p>");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "a\nb\nc");

  // Inside attributes too.
  auto doc2 = ParseDoc("<div title=\"x\r\ny\"></div>");
  dom::Element* div = dom::QuerySelector(*doc2, "div");
  ASSERT_NE(div, nullptr);
  EXPECT_EQ(div->GetAttribute("title").value(), "x\ny");
}

// ---- EOF in tag (eof-in-tag) ---------------------------------------------

TEST(HtmlTest, UnclosedTagAtEofIsDropped)
{
  // An unterminated <div at EOF is ignored entirely (eof-in-tag), not emitted
  // as a div element.
  auto doc = ParseDoc("<p>a<div");
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "div").size(), 0u);
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "a");

  // Unquoted attribute value at EOF: tag dropped, value not created.
  auto doc2 = ParseDoc("<div a=");
  EXPECT_EQ(dom::QuerySelectorAll(*doc2, "div").size(), 0u);
}

// ---- Character references in unquoted attribute values --------------------

TEST(HtmlTest, CharacterReferenceInUnquotedAttribute)
{
  // A '&' in an unquoted attribute value starts a character reference
  // (13.2.5.38).
  auto doc = ParseDoc("<div href=a&amp;b></div>");
  dom::Element* div = dom::QuerySelector(*doc, "div");
  ASSERT_NE(div, nullptr);
  EXPECT_EQ(div->GetAttribute("href").value(), "a&b");

  auto doc2 = ParseDoc("<div href=a&#65;b></div>");
  dom::Element* div2 = dom::QuerySelector(*doc2, "div");
  ASSERT_NE(div2, nullptr);
  EXPECT_EQ(div2->GetAttribute("href").value(), "aAb");
}

// A legacy no-semicolon named reference followed by '=' inside an attribute
// is emitted literally (13.2.5.78 "for historical reasons").
TEST(HtmlTest, LegacyReferenceBeforeEqualsInAttributeIsLiteral)
{
  auto doc = ParseDoc("<div title=\"&copy=x\"></div>");
  dom::Element* div = dom::QuerySelector(*doc, "div");
  ASSERT_NE(div, nullptr);
  EXPECT_EQ(div->GetAttribute("title").value(), "&copy=x");

  // In text content the same sequence is resolved (©=x).
  auto doc2 = ParseDoc("<p>&copy=x</p>");
  dom::Element* p = dom::QuerySelector(*doc2, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "\xC2\xA9=x");
}

// ---- DOCTYPE public/system identifiers ------------------------------------

TEST(HtmlTest, DoctypePublicSystemIdentifiers)
{
  // A doctype with PUBLIC/SYSTEM identifiers must not confuse the parser;
  // the skeleton is still built and the body follows.
  auto doc = ParseDoc("<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\" "
                      "\"http://www.w3.org/TR/html4/strict.dtd\"><p>x</p>");
  dom::Element* html = doc->document_element();
  ASSERT_NE(html, nullptr);
  EXPECT_EQ(html->tag_name(), "html");
  EXPECT_EQ(html->child_count(), 2u); // head + body
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "x");

  auto doc2 = ParseDoc("<!DOCTYPE html SYSTEM \"about:legacy-compat\"><p>y</p>");
  dom::Element* p2 = dom::QuerySelector(*doc2, "p");
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(p2->TextContent(), "y");

  // Single-quoted identifiers.
  auto doc3 = ParseDoc("<!DOCTYPE html PUBLIC '-//W3C//DTD XHTML 1.0//EN'>z");
  dom::Element* body = dom::QuerySelector(*doc3, "body");
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(body->TextContent(), "z");
}

// A stray doctype after the head is a parse error and is ignored, not reset
// back to "before html" (which would create a second html element).
TEST(HtmlTest, StrayDoctypeIsIgnored)
{
  auto doc = ParseDoc("<html><head></head><body>x<!DOCTYPE html>y</body></html>");
  dom::Element* html = doc->document_element();
  ASSERT_NE(html, nullptr);
  EXPECT_EQ(html->tag_name(), "html");
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "html").size(), 1u);
  dom::Element* body = dom::QuerySelector(*doc, "body");
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(body->TextContent(), "xy");
}

// ---- PLAINTEXT element ----------------------------------------------------

TEST(HtmlTest, PlaintextElementConsumesRest)
{
  auto doc = ParseDoc("<p>before<plaintext>rest <div> not a tag</plaintext>after");
  dom::Element* plaintext = dom::QuerySelector(*doc, "plaintext");
  ASSERT_NE(plaintext, nullptr);
  EXPECT_EQ(plaintext->TextContent(), "rest <div> not a tag</plaintext>after");
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "div").size(), 0u);
}

// ---- Raw text elements (xmp, iframe, noembed) -----------------------------

TEST(HtmlTest, RawTextXmpIframeNoembed)
{
  auto doc = ParseDoc("<xmp><p>raw</xmp><p>after</p>");
  dom::Element* xmp = dom::QuerySelector(*doc, "xmp");
  ASSERT_NE(xmp, nullptr);
  EXPECT_EQ(xmp->TextContent(), "<p>raw");
  dom::Element* p = dom::QuerySelector(*doc, "body p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "after");

  auto doc2 = ParseDoc("<iframe><div>hi</iframe><p>x</p>");
  dom::Element* iframe = dom::QuerySelector(*doc2, "iframe");
  ASSERT_NE(iframe, nullptr);
  EXPECT_EQ(iframe->TextContent(), "<div>hi");
  EXPECT_EQ(dom::QuerySelectorAll(*doc2, "div").size(), 0u);

  auto doc3 = ParseDoc("<noembed><b>raw</noembed><p>y</p>");
  dom::Element* noembed = dom::QuerySelector(*doc3, "noembed");
  ASSERT_NE(noembed, nullptr);
  EXPECT_EQ(noembed->TextContent(), "<b>raw");
  EXPECT_EQ(dom::QuerySelectorAll(*doc3, "b").size(), 0u);
}

// ---- hr / center / dd / dt close a p --------------------------------------

TEST(HtmlTest, HrCenterCloseOpenP)
{
  // <hr> and <center> close an open <p> (13.2.6.4.7).
  auto doc = ParseDoc("<p>a<hr>b");
  const auto ps = dom::QuerySelectorAll(*doc, "p");
  ASSERT_EQ(ps.size(), 1u);
  EXPECT_EQ(ps[0]->TextContent(), "a");
  dom::Element* hr = dom::QuerySelector(*doc, "hr");
  ASSERT_NE(hr, nullptr);
  EXPECT_EQ(hr->parent(), dom::QuerySelector(*doc, "body"));

  auto doc2 = ParseDoc("<p>a<center>b");
  const auto ps2 = dom::QuerySelectorAll(*doc2, "p");
  ASSERT_EQ(ps2.size(), 1u);
  EXPECT_EQ(ps2[0]->TextContent(), "a");
  dom::Element* center = dom::QuerySelector(*doc2, "center");
  ASSERT_NE(center, nullptr);
  EXPECT_EQ(center->parent(), dom::QuerySelector(*doc2, "body"));
}

TEST(HtmlTest, DdDtCloseEachOther)
{
  auto doc = ParseDoc("<dl><dt>a<dd>b<dt>c</dl>");
  const auto dts = dom::QuerySelectorAll(*doc, "dt");
  const auto dds = dom::QuerySelectorAll(*doc, "dd");
  ASSERT_EQ(dts.size(), 2u);
  ASSERT_EQ(dds.size(), 1u);
  EXPECT_EQ(dts[0]->TextContent(), "a");
  EXPECT_EQ(dds[0]->TextContent(), "b");
  EXPECT_EQ(dts[1]->TextContent(), "c");
  // dd is a sibling of both dt elements (all under dl).
  dom::Element* dl = dom::QuerySelector(*doc, "dl");
  ASSERT_NE(dl, nullptr);
  EXPECT_EQ(dl->child_count(), 3u);
}

// ---- after-head content goes into head ------------------------------------

TEST(HtmlTest, AfterHeadElementsGoIntoHead)
{
  // base/link/meta/style/script after </head> are still processed with the
  // "in head" rules and end up in the head element (13.2.6.4.6).
  auto doc = ParseDoc("<head></head><link rel=\"stylesheet\" href=\"a.css\">"
                      "<meta charset=\"utf-8\"><body>x</body>");
  dom::Element* head = dom::QuerySelector(*doc, "head");
  ASSERT_NE(head, nullptr);
  EXPECT_EQ(dom::QuerySelectorAll(*head, "link").size(), 1u);
  EXPECT_EQ(dom::QuerySelectorAll(*head, "meta").size(), 1u);
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "body link").size(), 0u);
}

// ---- Self-closing flag on non-void elements is ignored --------------------

TEST(HtmlTest, SelfClosingOnNonVoidElementIsIgnored)
{
  // <div/> is a parse error; the element stays open (no self-closing pop).
  auto doc = ParseDoc("<div/>x");
  dom::Element* div = dom::QuerySelector(*doc, "div");
  ASSERT_NE(div, nullptr);
  EXPECT_EQ(div->TextContent(), "x");
}

// ---- Tables ---------------------------------------------------------------

TEST(HtmlTest, TableStructure)
{
  auto doc = ParseDoc("<table><caption>Cap</caption>"
                      "<tr><td>A</td><td>B</td></tr>"
                      "<tr><td>C</td></tr></table>");
  dom::Element* table = dom::QuerySelector(*doc, "table");
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "caption").size(), 1u);
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "tbody").size(), 1u);
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "tr").size(), 2u);
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "td").size(), 3u);
  EXPECT_EQ(table->TextContent(), "CapABC");
}

TEST(HtmlTest, TableWithTheadTbodyTfoot)
{
  auto doc = ParseDoc("<table><thead><tr><th>H</th></tr></thead>"
                      "<tbody><tr><td>D</td></tr></tbody>"
                      "<tfoot><tr><td>F</td></tr></tfoot></table>");
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "thead").size(), 1u);
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "tbody").size(), 1u);
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "tfoot").size(), 1u);
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "th").size(), 1u);
}

TEST(HtmlTest, TableColgroup)
{
  auto doc = ParseDoc("<table><colgroup><col><col></colgroup>"
                      "<tr><td>a</td><td>b</td></tr></table>");
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "colgroup").size(), 1u);
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "col").size(), 2u);
  dom::Element* colgroup = dom::QuerySelector(*doc, "colgroup");
  ASSERT_NE(colgroup, nullptr);
  EXPECT_EQ(colgroup->child_count(), 2u);
}

TEST(HtmlTest, FosterParentingTextBeforeTable)
{
  // Text directly inside a table (not in a cell) is foster-parented to before
  // the table (13.2.6.4.10).
  auto doc = ParseDoc("<div>a<table>text<tr><td>c</td></tr></table>");
  dom::Element* div = dom::QuerySelector(*doc, "div");
  ASSERT_NE(div, nullptr);
  dom::Element* table = dom::QuerySelector(*doc, "table");
  ASSERT_NE(table, nullptr);
  // The fostered text is a sibling of the table, before it, inside the div.
  EXPECT_EQ(table->parent(), div);
  dom::Node* before = nullptr;
  for (dom::Node* child : div->ChildNodes()) {
    if (child == table) {
      break;
    }
    before = child;
  }
  ASSERT_NE(before, nullptr);
  EXPECT_EQ(before->node_type(), dom::NodeType::kText);
  EXPECT_EQ(before->TextContent(), "atext");
  EXPECT_EQ(div->TextContent(), "atextc");
}

TEST(HtmlTest, FosterParentingMisnestedElement)
{
  // A block element inside a table is foster-parented out.
  auto doc = ParseDoc("<table><div>hello</div><tr><td>c</td></tr></table>");
  dom::Element* body = dom::QuerySelector(*doc, "body");
  ASSERT_NE(body, nullptr);
  dom::Element* div = dom::QuerySelector(*body, "div");
  ASSERT_NE(div, nullptr);
  EXPECT_EQ(div->parent(), body);
  EXPECT_EQ(div->TextContent(), "hello");
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "table div").size(), 0u);
}

TEST(HtmlTest, TableWhitespaceStaysInRow)
{
  // Whitespace inside table structure is preserved in place; it must not
  // create extra implicit tbody elements.
  auto doc = ParseDoc("<table>\n  <tr>\n    <td>x</td>\n  </tr>\n</table>");
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "tbody").size(), 1u);
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "tr").size(), 1u);
}

TEST(HtmlTest, NestedTableInsideCell)
{
  auto doc = ParseDoc("<table><tr><td>a<table><tr><td>b</td></tr></table>c</td></tr></table>");
  // The inner table is nested inside the outer td, not a sibling.
  dom::Element* inner = dom::QuerySelector(*doc, "table table");
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->parent(), dom::QuerySelector(*doc, "td"));
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "table").size(), 2u);
  dom::Element* outer = dom::QuerySelector(*doc, "table");
  ASSERT_NE(outer, nullptr);
  EXPECT_EQ(dom::QuerySelectorAll(*outer, "td").size(), 2u);
}

TEST(HtmlTest, CloseTableCellStartsNewRow)
{
  // Two consecutive cells: the first cell is closed by the second td.
  auto doc = ParseDoc("<table><tr><td>a<td>b</tr></table>");
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "tr").size(), 1u);
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "td").size(), 2u);
  dom::Element* tr = dom::QuerySelector(*doc, "tr");
  ASSERT_NE(tr, nullptr);
  EXPECT_EQ(tr->child_count(), 2u);
}

TEST(HtmlTest, ImplicitTbodyForBareRows)
{
  // A bare <tr> (or <td>) is wrapped in an implicit tbody.
  auto doc = ParseDoc("<table><tr><td>a</td></tr></table>");
  EXPECT_EQ(dom::QuerySelectorAll(*doc, "tbody").size(), 1u);
  dom::Element* tbody = dom::QuerySelector(*doc, "tbody");
  ASSERT_NE(tbody, nullptr);
  EXPECT_EQ(tbody->parent(), dom::QuerySelector(*doc, "table"));
}

// ---- doctype quirks flag is consumed (via tokenizer) -----------------------

TEST(HtmlTest, DoctypeQuirksConsumedWithoutBreakingParse)
{
  auto doc = ParseDoc("<!DOCTYPE html PUBLIC \"x\" \"y\"><p>z</p>");
  dom::Element* p = dom::QuerySelector(*doc, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "z");
}

} // namespace
} // namespace neko::html
