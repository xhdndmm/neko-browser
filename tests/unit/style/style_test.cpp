#include "neko/dom/query.h"
#include "neko/html/parser.h"
#include "neko/style/computed_style.h"
#include "neko/style/style_engine.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>

namespace neko::style {
namespace {

std::unique_ptr<dom::Document> MakeDoc(std::string_view html)
{
  return html::Parser(html).Parse();
}

// Returns the computed style of the first element matching |selector|.
const ComputedStyle& Style(StyleEngine& engine, dom::Document& doc, std::string_view selector)
{
  const dom::Element* element = dom::QuerySelector(doc, selector);
  return engine.StyleFor(*element);
}

TEST(StyleTest, UaDefaults)
{
  auto doc = MakeDoc("<html><head><style>p{}</style></head><body><div>x</div><span>y</span><p>z</"
                     "p></body></html>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  EXPECT_EQ(Style(engine, *doc, "body").display, Display::kBlock);
  EXPECT_EQ(Style(engine, *doc, "div").display, Display::kBlock);
  EXPECT_EQ(Style(engine, *doc, "span").display, Display::kInline);
  EXPECT_EQ(Style(engine, *doc, "style").display, Display::kNone);
  // p margin: 1em = 16px at default font size.
  const ComputedStyle& p = Style(engine, *doc, "p");
  EXPECT_FLOAT_EQ(p.margin_top.value, 16.0f);
  EXPECT_FLOAT_EQ(p.margin_bottom.value, 16.0f);
  EXPECT_FLOAT_EQ(p.font_size, 16.0f);
}

TEST(StyleTest, DisplayInlineBlock)
{
  // display:inline-block must be its own Display kind (not folded into
  // display:inline) so layout can treat it as an atomic block box.
  auto doc = MakeDoc("<body><div style=\"display:block\">b</div>"
                     "<span style=\"display:inline-block\">ib</span></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  EXPECT_EQ(Style(engine, *doc, "body").display, Display::kBlock);
  EXPECT_EQ(Style(engine, *doc, "div").display, Display::kBlock);
  EXPECT_EQ(Style(engine, *doc, "span").display, Display::kInlineBlock);
}

TEST(StyleTest, FloatParsesLeftRightNone)
{
  auto doc = MakeDoc("<body><div style=\"float:left\">a</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  EXPECT_EQ(Style(engine, *doc, "div").floating, Float::kLeft);

  auto doc2 = MakeDoc("<body><div style=\"float:right\">b</div></body>");
  StyleEngine engine2;
  engine2.ApplyStyles(*doc2);
  EXPECT_EQ(Style(engine2, *doc2, "div").floating, Float::kRight);

  auto doc3 = MakeDoc("<body><div>c</div></body>");
  StyleEngine engine3;
  engine3.ApplyStyles(*doc3);
  EXPECT_EQ(Style(engine3, *doc3, "div").floating, Float::kNone);
}

TEST(StyleTest, DisplayFlexParses)
{
  auto doc = MakeDoc("<body><div style=\"display:flex\">a</div>"
                     "<span style=\"display:inline-flex\">b</span></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  EXPECT_EQ(Style(engine, *doc, "div").display, Display::kFlex);
  EXPECT_EQ(Style(engine, *doc, "span").display, Display::kInlineFlex);
}

TEST(StyleTest, FlexContainerPropertiesParse)
{
  auto doc = MakeDoc("<body><div style=\"display:flex; flex-direction:column; flex-wrap:wrap; "
                     "justify-content:space-between; align-items:center; align-content:flex-end; "
                     "gap:12px 8px; row-gap:5px\">a</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  const ComputedStyle& s = Style(engine, *doc, "div");
  EXPECT_EQ(s.flex_direction, FlexDirection::kColumn);
  EXPECT_EQ(s.flex_wrap, FlexWrap::kWrap);
  EXPECT_EQ(s.justify_content, JustifyContent::kSpaceBetween);
  EXPECT_EQ(s.align_items, AlignItems::kCenter);
  EXPECT_EQ(s.align_content, AlignContent::kFlexEnd);
  // gap shorthand is overridden by the later row-gap declaration.
  EXPECT_FLOAT_EQ(s.row_gap, 5.0f);
  EXPECT_FLOAT_EQ(s.column_gap, 8.0f);
}

TEST(StyleTest, FlexItemPropertiesParse)
{
  auto doc = MakeDoc("<body><div style=\"display:flex\"><p style=\"flex-grow:2; flex-shrink:0; "
                     "flex-basis:120px\">x</p></div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  const ComputedStyle& p = Style(engine, *doc, "p");
  EXPECT_FLOAT_EQ(p.flex_grow, 2.0f);
  EXPECT_FLOAT_EQ(p.flex_shrink, 0.0f);
  ASSERT_TRUE(p.flex_basis.has_value());
  EXPECT_FLOAT_EQ(p.flex_basis.value().value, 120.0f);
  EXPECT_FALSE(p.flex_basis.value().percent);
}

TEST(StyleTest, FlexShorthandSingleNumber)
{
  // flex: 1 == flex: 1 1 0.
  auto doc = MakeDoc("<body><div style=\"display:flex\"><p style=\"flex:1\">x</p></div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  const ComputedStyle& p = Style(engine, *doc, "p");
  EXPECT_FLOAT_EQ(p.flex_grow, 1.0f);
  EXPECT_FLOAT_EQ(p.flex_shrink, 1.0f);
  ASSERT_TRUE(p.flex_basis.has_value());
  EXPECT_FLOAT_EQ(p.flex_basis.value().value, 0.0f);
}

TEST(StyleTest, HeadingIsBlock)
{
  auto doc = MakeDoc("<body><h1>x</h1><h2>y</h2></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  EXPECT_EQ(Style(engine, *doc, "h1").display, Display::kBlock);
  EXPECT_EQ(Style(engine, *doc, "h2").display, Display::kBlock);
}

TEST(StyleTest, SpecificityCascade)
{
  auto doc = MakeDoc("<style>p { color: red; font-size: 20px; } .note { color: blue; }</style>"
                     "<body><p class=\"note\">x</p></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& style = Style(engine, *doc, "p");
  // .note (0,1,0) beats p (0,0,1) for color; font-size comes from p.
  ASSERT_TRUE(style.color.has_value());
  EXPECT_EQ(style.color.value(), (css::Color{0, 0, 255, 255}));
  EXPECT_FLOAT_EQ(style.font_size, 20.0f);
}

TEST(StyleTest, InlineStyleBeatsSelectors)
{
  auto doc = MakeDoc("<style>.note { color: blue; }</style>"
                     "<body><p class=\"note\" style=\"color: green\">x</p></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& style = Style(engine, *doc, "p");
  ASSERT_TRUE(style.color.has_value());
  EXPECT_EQ(style.color.value(), (css::Color{0, 128, 0, 255}));
}

TEST(StyleTest, ImportantBeatsInline)
{
  auto doc = MakeDoc("<style>.note { color: blue !important; }</style>"
                     "<body><p class=\"note\" style=\"color: green\">x</p></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& style = Style(engine, *doc, "p");
  ASSERT_TRUE(style.color.has_value());
  EXPECT_EQ(style.color.value(), (css::Color{0, 0, 255, 255}));
}

TEST(StyleTest, Inheritance)
{
  auto doc = MakeDoc("<body><div style=\"color: red; font-size: 24px\"><p>x</p></div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& p = Style(engine, *doc, "p");
  ASSERT_TRUE(p.color.has_value());
  EXPECT_EQ(p.color.value(), (css::Color{255, 0, 0, 255}));
  EXPECT_FLOAT_EQ(p.font_size, 24.0f);
}

TEST(StyleTest, EmFontSizeResolution)
{
  auto doc = MakeDoc(
      "<body><div style=\"font-size: 16px\"><p style=\"font-size: 2em\">x</p></div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& p = Style(engine, *doc, "p");
  EXPECT_FLOAT_EQ(p.font_size, 32.0f);
}

TEST(StyleTest, LineHeightScalesWithFontSize)
{
  auto doc = MakeDoc("<body><h1>x</h1></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& h1 = Style(engine, *doc, "h1");
  EXPECT_FLOAT_EQ(h1.font_size, 32.0f);   // 2em
  EXPECT_FLOAT_EQ(h1.line_height, 38.4f); // 32 * 1.2 (was stuck at 19.2)
}

TEST(StyleTest, FontStyleParsesItalic)
{
  auto doc = MakeDoc(
      "<body><p>plain</p><div style=\"font-style: italic\">it</div><i>ua</i><b>bold</b></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  EXPECT_FALSE(Style(engine, *doc, "p").font_italic);
  EXPECT_TRUE(Style(engine, *doc, "div").font_italic);  // inline style
  EXPECT_TRUE(Style(engine, *doc, "i").font_italic);    // UA: i, em
  EXPECT_EQ(Style(engine, *doc, "b").font_weight, 700); // UA: b, strong
  EXPECT_EQ(Style(engine, *doc, "p").font_weight, 400);
}

TEST(StyleTest, WidthHeightAndBackground)
{
  auto doc = MakeDoc(
      "<body><div style=\"width: 200px; height: 100px; background-color: #ff0000\">x</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& style = Style(engine, *doc, "div");
  ASSERT_TRUE(style.width.has_value());
  EXPECT_FLOAT_EQ(style.width.value().value, 200.0f);
  ASSERT_TRUE(style.height.has_value());
  EXPECT_FLOAT_EQ(style.height.value().value, 100.0f);
  ASSERT_TRUE(style.background_color.has_value());
  EXPECT_EQ(style.background_color.value(), (css::Color{255, 0, 0, 255}));
}

TEST(StyleTest, MarginShorthand)
{
  auto doc = MakeDoc("<body><div style=\"margin: 1em 2em\">x</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& style = Style(engine, *doc, "div");
  EXPECT_FLOAT_EQ(style.margin_top.value, 16.0f);
  EXPECT_FLOAT_EQ(style.margin_bottom.value, 16.0f);
  EXPECT_FLOAT_EQ(style.margin_left.value, 32.0f);
  EXPECT_FLOAT_EQ(style.margin_right.value, 32.0f);
}

TEST(StyleTest, PaddingAndBorder)
{
  auto doc = MakeDoc("<body><div style=\"padding: 10px; border: 2px solid #000\">x</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& style = Style(engine, *doc, "div");
  EXPECT_FLOAT_EQ(style.padding_top.value, 10.0f);
  EXPECT_FLOAT_EQ(style.padding_left.value, 10.0f);
  EXPECT_FLOAT_EQ(style.border_top.value, 2.0f);
  EXPECT_EQ(style.border_style, BorderStyle::kSolid);
  ASSERT_TRUE(style.border_color.has_value());
  EXPECT_EQ(style.border_color.value(), (css::Color{0, 0, 0, 255}));
}

TEST(StyleTest, TextAlign)
{
  auto doc = MakeDoc("<body><p style=\"text-align: center\">x</p></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  EXPECT_EQ(Style(engine, *doc, "p").text_align, TextAlign::kCenter);
}

TEST(StyleTest, MediaQueryScreenApplies)
{
  auto doc = MakeDoc("<style>@media screen { p { color: rgb(1,2,3); } }</style>"
                     "<body><p>x</p></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  const ComputedStyle& style = Style(engine, *doc, "p");
  ASSERT_TRUE(style.color.has_value());
  EXPECT_EQ(style.color.value(), (css::Color{1, 2, 3, 255}));
}

TEST(StyleTest, MediaQueryPrintSkipped)
{
  auto doc = MakeDoc("<style>@media print { p { color: red; } }</style>"
                     "<body><p>x</p></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  EXPECT_FALSE(Style(engine, *doc, "p").color.has_value());
}

} // namespace
} // namespace neko::style
