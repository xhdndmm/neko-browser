#include "neko/css/color.h"
#include "neko/css/parser.h"
#include "neko/css/selector.h"
#include "neko/css/tokenizer.h"
#include "neko/css/value.h"
#include "neko/dom/query.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>

namespace neko::css {
namespace {

// Builds a small DOM tree for selector matching tests.
std::unique_ptr<dom::Document> MakeTree()
{
  auto doc = std::make_unique<dom::Document>();
  auto html = std::make_unique<dom::Element>("html");
  auto body = std::make_unique<dom::Element>("body");
  auto main = std::make_unique<dom::Element>("div");
  main->SetAttribute("id", "main");
  main->SetAttribute("class", "content note");
  main->SetAttribute("lang", "en-US");
  auto p1 = std::make_unique<dom::Element>("p");
  p1->SetAttribute("class", "note");
  main->AppendChild(std::move(p1));
  auto p2 = std::make_unique<dom::Element>("p");
  dom::Element* p2_raw = p2.get();
  main->AppendChild(std::move(p2));
  auto span = std::make_unique<dom::Element>("span");
  p2_raw->AppendChild(std::move(span));
  body->AppendChild(std::move(main));
  html->AppendChild(std::move(body));
  doc->AppendChild(std::move(html));
  return doc;
}

dom::Element* ById(dom::Document& doc, std::string_view id)
{
  return dom::QuerySelector(doc, std::string("#") + std::string(id));
}

TEST(CssColorTest, HexColors)
{
  const auto c1 = ParseColor("#f00");
  ASSERT_TRUE(c1.has_value());
  EXPECT_EQ(c1.value(), (Color{255, 0, 0, 255}));

  const auto c2 = ParseColor("#ff0000");
  ASSERT_TRUE(c2.has_value());
  EXPECT_EQ(c2.value(), (Color{255, 0, 0, 255}));

  const auto c3 = ParseColor("#00ff0080");
  ASSERT_TRUE(c3.has_value());
  EXPECT_EQ(c3.value(), (Color{0, 255, 0, 128}));

  EXPECT_FALSE(ParseColor("#xyz").has_value());
  EXPECT_FALSE(ParseColor("#ff").has_value());
}

TEST(CssColorTest, RgbFunction)
{
  const auto c1 = ParseColor("rgb(255, 0, 128)");
  ASSERT_TRUE(c1.has_value());
  EXPECT_EQ(c1.value(), (Color{255, 0, 128, 255}));

  const auto c2 = ParseColor("rgba(0, 128, 255, 0.5)");
  ASSERT_TRUE(c2.has_value());
  EXPECT_EQ(c2.value().r, 0);
  EXPECT_EQ(c2.value().g, 128);
  EXPECT_EQ(c2.value().b, 255);
  EXPECT_EQ(c2.value().a, 128);

  const auto c3 = ParseColor("rgb(100%, 0%, 50%)");
  ASSERT_TRUE(c3.has_value());
  EXPECT_EQ(c3.value(), (Color{255, 0, 128, 255}));

  EXPECT_FALSE(ParseColor("rgb(300)").has_value());
}

TEST(CssColorTest, NamedColors)
{
  EXPECT_EQ(ParseColor("red").value(), (Color{255, 0, 0, 255}));
  EXPECT_EQ(ParseColor("black").value(), (Color{0, 0, 0, 255}));
  EXPECT_EQ(ParseColor("white").value(), (Color{255, 255, 255, 255}));
  EXPECT_EQ(ParseColor("transparent").value(), (Color{0, 0, 0, 0}));
  EXPECT_EQ(ParseColor("Blue").value(), (Color{0, 0, 255, 255}));
  EXPECT_FALSE(ParseColor("notacolor").has_value());
}

TEST(CssValueTest, ParseValues)
{
  CssValue v1 = ParseCssValue("red");
  EXPECT_EQ(v1.type, CssValue::Type::kColor);

  CssValue v2 = ParseCssValue("10px");
  EXPECT_EQ(v2.type, CssValue::Type::kLength);
  EXPECT_EQ(v2.value, 10.0f);
  EXPECT_EQ(v2.unit, "px");

  CssValue v3 = ParseCssValue("1.5em");
  EXPECT_EQ(v3.type, CssValue::Type::kLength);
  EXPECT_EQ(v3.value, 1.5f);
  EXPECT_EQ(v3.unit, "em");

  CssValue v4 = ParseCssValue("50%");
  EXPECT_EQ(v4.type, CssValue::Type::kLength);
  EXPECT_TRUE(v4.is_percent);

  CssValue v5 = ParseCssValue("42");
  EXPECT_EQ(v5.type, CssValue::Type::kNumber);
  EXPECT_EQ(v5.number, 42.0f);

  CssValue v6 = ParseCssValue("block");
  EXPECT_EQ(v6.type, CssValue::Type::kKeyword);
  EXPECT_TRUE(IsKeyword(v6, "block"));
  EXPECT_FALSE(IsKeyword(v6, "inline"));
}

TEST(CssSelectorTest, TypeAndClassAndId)
{
  auto doc = MakeTree();
  dom::Element* main = ById(*doc, "main");
  ASSERT_NE(main, nullptr);

  std::vector<ComplexSelector> s1 = ParseSelectorList("div");
  ASSERT_EQ(s1.size(), 1u);
  EXPECT_TRUE(MatchesSelector(*main, s1[0]));

  std::vector<ComplexSelector> s2 = ParseSelectorList(".content");
  ASSERT_EQ(s2.size(), 1u);
  EXPECT_TRUE(MatchesSelector(*main, s2[0]));

  std::vector<ComplexSelector> s3 = ParseSelectorList("div#main.note");
  ASSERT_EQ(s3.size(), 1u);
  EXPECT_TRUE(MatchesSelector(*main, s3[0]));

  std::vector<ComplexSelector> s4 = ParseSelectorList("p");
  ASSERT_EQ(s4.size(), 1u);
  EXPECT_FALSE(MatchesSelector(*main, s4[0]));
}

TEST(CssSelectorTest, AttributeSelectors)
{
  auto doc = MakeTree();
  dom::Element* main = ById(*doc, "main");

  EXPECT_TRUE(MatchesSelector(*main, ParseSelectorList("[lang]")[0]));
  EXPECT_TRUE(MatchesSelector(*main, ParseSelectorList("[lang=en-US]")[0]));
  EXPECT_TRUE(MatchesSelector(*main, ParseSelectorList("[lang|=en]")[0]));
  EXPECT_TRUE(MatchesSelector(*main, ParseSelectorList("[class~=note]")[0]));
  EXPECT_TRUE(MatchesSelector(*main, ParseSelectorList("[lang^=en]")[0]));
  EXPECT_TRUE(MatchesSelector(*main, ParseSelectorList("[class$=note]")[0]));
  EXPECT_TRUE(MatchesSelector(*main, ParseSelectorList("[lang*=US]")[0]));
  EXPECT_FALSE(MatchesSelector(*main, ParseSelectorList("[lang=fr]")[0]));
}

TEST(CssSelectorTest, AttributeEmptyValueNeverMatchesSubstringOps)
{
  auto doc = MakeTree();
  dom::Element* main = ById(*doc, "main");
  ASSERT_NE(main, nullptr);
  ASSERT_TRUE(main->GetAttribute("lang").has_value());

  // An empty value with ^= $= *= |= ~= is a substring/prefix/suffix of every
  // string; per CSS Selectors these must never match.
  EXPECT_FALSE(MatchesSelector(*main, ParseSelectorList("[lang^=\"\"]")[0]));
  EXPECT_FALSE(MatchesSelector(*main, ParseSelectorList("[lang$=\"\"]")[0]));
  EXPECT_FALSE(MatchesSelector(*main, ParseSelectorList("[lang*=\"\"]")[0]));
  EXPECT_FALSE(MatchesSelector(*main, ParseSelectorList("[lang|=\"\"]")[0]));
  EXPECT_FALSE(MatchesSelector(*main, ParseSelectorList("[lang~=\"\"]")[0]));
}

TEST(CssSelectorTest, AttributeEqualsEmptyValueMatchesEmpty)
{
  auto doc = MakeTree();
  dom::Element* div = ById(*doc, "main");
  ASSERT_NE(div, nullptr);
  div->SetAttribute("data-empty", "");

  // "=" with an empty value matches an attribute whose value is empty.
  EXPECT_TRUE(MatchesSelector(*div, ParseSelectorList("[data-empty=\"\"]")[0]));
}

TEST(CssSelectorTest, Combinators)
{
  auto doc = MakeTree();
  dom::Element* span = dom::QuerySelector(*doc, "span");
  ASSERT_NE(span, nullptr);

  EXPECT_TRUE(MatchesSelector(*span, ParseSelectorList("body div p span")[0]));
  EXPECT_TRUE(MatchesSelector(*span, ParseSelectorList("p > span")[0]));
  EXPECT_FALSE(MatchesSelector(*span, ParseSelectorList("body > span")[0]));
  EXPECT_TRUE(MatchesSelector(*span, ParseSelectorList("p + p span")[0]));
}

TEST(CssSelectorTest, PseudoClasses)
{
  auto doc = MakeTree();
  const std::vector<dom::Element*> ps = dom::QuerySelectorAll(*doc, "p");
  ASSERT_EQ(ps.size(), 2u);
  dom::Element* p1 = ps[0];
  dom::Element* p2 = ps[1];

  EXPECT_TRUE(MatchesSelector(*p1, ParseSelectorList("p:first-child")[0]));
  EXPECT_FALSE(MatchesSelector(*p2, ParseSelectorList("p:first-child")[0]));
  EXPECT_TRUE(MatchesSelector(*p2, ParseSelectorList("p:last-child")[0]));
  EXPECT_TRUE(MatchesSelector(*p1, ParseSelectorList("p:nth-child(1)")[0]));
  EXPECT_TRUE(MatchesSelector(*p2, ParseSelectorList("p:nth-child(2)")[0]));
  EXPECT_TRUE(MatchesSelector(*p2, ParseSelectorList("p:nth-child(2n)")[0]));
}

TEST(CssSelectorTest, NthChildWithPlusParses)
{
  // The '+' inside :nth-child(An+B) must not be mistaken for a sibling
  // combinator (regression: the complex-selector scanner only tracked []).
  auto doc = MakeTree();
  const std::vector<dom::Element*> ps = dom::QuerySelectorAll(*doc, "p");
  ASSERT_EQ(ps.size(), 2u);
  dom::Element* p1 = ps[0];
  dom::Element* p2 = ps[1];
  EXPECT_EQ(ParseSelectorList("p:nth-child(2n+1)").size(), 1u);
  EXPECT_EQ(ParseSelectorList("p:nth-child(2n + 1)").size(), 1u);
  EXPECT_EQ(ParseSelectorList("p:nth-child(-n+3)").size(), 1u);
  EXPECT_TRUE(MatchesSelector(*p1, ParseSelectorList("p:nth-child(2n+1)")[0]));
  EXPECT_FALSE(MatchesSelector(*p2, ParseSelectorList("p:nth-child(2n+1)")[0]));
}

TEST(CssSelectorTest, Specificity)
{
  auto doc = MakeTree();
  dom::Element* main = ById(*doc, "main");

  const Specificity type = MatchingSpecificity(*main, "div");
  EXPECT_EQ(type, (Specificity{0, 0, 1}));

  const Specificity cls = MatchingSpecificity(*main, ".content");
  EXPECT_EQ(cls, (Specificity{0, 1, 0}));

  const Specificity id = MatchingSpecificity(*main, "#main");
  EXPECT_EQ(id, (Specificity{1, 0, 0}));

  const Specificity combined = MatchingSpecificity(*main, "div#main.content");
  EXPECT_EQ(combined, (Specificity{1, 1, 1}));

  const Specificity none = MatchingSpecificity(*main, "p");
  EXPECT_EQ(none, (Specificity{0, 0, 0}));
}

TEST(CssParserTest, ParseStyleSheet)
{
  const StyleSheet sheet = ParseStyleSheet("p { color: red; margin: 0; }\n"
                                           ".note { font-size: 14px !important; }\n"
                                           "@media screen { div { display: block; } }\n"
                                           "@import url(other.css);");

  ASSERT_EQ(sheet.rules.size(), 2u);
  EXPECT_EQ(sheet.rules[0].selectors.size(), 1u);
  EXPECT_EQ(sheet.rules[0].declarations.size(), 2u);
  EXPECT_EQ(sheet.rules[0].declarations[0].property, "color");
  EXPECT_EQ(sheet.rules[0].declarations[0].value, "red");
  EXPECT_FALSE(sheet.rules[0].declarations[0].important);

  EXPECT_EQ(sheet.rules[1].declarations[0].property, "font-size");
  EXPECT_EQ(sheet.rules[1].declarations[0].value, "14px");
  EXPECT_TRUE(sheet.rules[1].declarations[0].important);

  ASSERT_EQ(sheet.at_rules.size(), 2u);
  EXPECT_EQ(sheet.at_rules[0].name, "media");
  ASSERT_EQ(sheet.at_rules[0].rules.size(), 1u);
  EXPECT_EQ(sheet.at_rules[0].rules[0].declarations[0].property, "display");
  EXPECT_EQ(sheet.at_rules[1].name, "import");
}

TEST(CssParserTest, SelectorList)
{
  const StyleSheet sheet = ParseStyleSheet("h1, h2, .title { color: black; }");
  ASSERT_EQ(sheet.rules.size(), 1u);
  EXPECT_EQ(sheet.rules[0].selectors.size(), 3u);
}

TEST(CssParserTest, DeclarationBlock)
{
  const std::vector<Declaration> decls =
      ParseDeclarationBlock("color: red; margin: 4px; background: #fff;");
  ASSERT_EQ(decls.size(), 3u);
  EXPECT_EQ(decls[0].property, "color");
  EXPECT_EQ(decls[0].value, "red");
  EXPECT_EQ(decls[2].value, "#fff");
}

TEST(CssParserTest, MalformedInputIsTolerated)
{
  const StyleSheet sheet = ParseStyleSheet("p { color: red } div {");
  // The first rule parses; the second (unclosed) rule is tolerated.
  ASSERT_GE(sheet.rules.size(), 1u);
  EXPECT_EQ(sheet.rules[0].declarations[0].property, "color");
}

// Regression: a stray ";" after a selector (no block follows) must not stall
// the parser in an infinite loop pushing empty rules.  Real pages (e.g.
// Baidu's homepage) contain "foo;" fragments and @font-face bodies made of
// raw declarations separated by ";".
TEST(CssParserTest, StraySemicolonAfterSelectorIsSkipped)
{
  const StyleSheet sheet = ParseStyleSheet("p; .note { color: red; }");
  // "p" is a valid type selector (empty declaration block), ".note" follows.
  ASSERT_EQ(sheet.rules.size(), 2u);
  EXPECT_EQ(sheet.rules[0].selectors.size(), 1u);
  EXPECT_TRUE(sheet.rules[0].declarations.empty());
  EXPECT_EQ(sheet.rules[1].selectors.size(), 1u);
  EXPECT_EQ(sheet.rules[1].declarations[0].property, "color");
}

TEST(CssParserTest, TrailingSemicolonDoesNotHang)
{
  const StyleSheet sheet = ParseStyleSheet("p { color: red };");
  ASSERT_EQ(sheet.rules.size(), 1u);
  EXPECT_EQ(sheet.rules[0].declarations[0].value, "red");
}

TEST(CssParserTest, FontFaceBodyDoesNotHang)
{
  // @font-face bodies are raw declarations separated by ";", not nested
  // qualified rules.  This previously stalled the at-rule loop forever.
  const StyleSheet sheet =
      ParseStyleSheet("@font-face{font-family:cIconfont;src:url('a.eot');src:url('b.woff2') "
                      "format('woff2')}.x{color:blue}");
  ASSERT_EQ(sheet.at_rules.size(), 1u);
  EXPECT_EQ(sheet.at_rules[0].name, "font-face");
  // The following rule must still be parsed (the block was consumed).
  ASSERT_EQ(sheet.rules.size(), 1u);
  EXPECT_EQ(sheet.rules[0].declarations[0].property, "color");
  EXPECT_EQ(sheet.rules[0].declarations[0].value, "blue");
}

TEST(CssParserTest, UnmatchedCloseBraceDoesNotHang)
{
  const StyleSheet sheet = ParseStyleSheet("} .a { color: red; }");
  ASSERT_EQ(sheet.rules.size(), 1u);
  EXPECT_EQ(sheet.rules[0].declarations[0].property, "color");
}

TEST(CssTokenizerTest, BasicTokens)
{
  Tokenizer tokenizer("div.class { color: red; margin: 10px; }");
  const std::vector<CssToken> tokens = tokenizer.Tokenize();
  EXPECT_GE(tokens.size(), 12u);
}

} // namespace
} // namespace neko::css
