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

TEST(StyleTest, FlexShorthandKeywords)
{
  // flex: none == flex: 0 0 auto (CSS Flexbox 1 §7.1).
  auto doc = MakeDoc("<body><div style=\"display:flex\"><p style=\"flex:none\">x</p></div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  const ComputedStyle& p = Style(engine, *doc, "p");
  EXPECT_FLOAT_EQ(p.flex_grow, 0.0f);
  EXPECT_FLOAT_EQ(p.flex_shrink, 0.0f);
  EXPECT_FALSE(p.flex_basis.has_value()); // auto

  // flex: auto == flex: 1 1 auto.
  auto doc2 =
      MakeDoc("<body><div style=\"display:flex\"><p style=\"flex:auto\">x</p></div></body>");
  StyleEngine engine2;
  engine2.ApplyStyles(*doc2);
  const ComputedStyle& p2 = Style(engine2, *doc2, "p");
  EXPECT_FLOAT_EQ(p2.flex_grow, 1.0f);
  EXPECT_FLOAT_EQ(p2.flex_shrink, 1.0f);
  EXPECT_FALSE(p2.flex_basis.has_value());
}

TEST(StyleTest, FlexShorthandTwoAndThreeValues)
{
  // flex: 2 30px -> grow 2, shrink 1 (default), basis 30px.
  auto doc =
      MakeDoc("<body><div style=\"display:flex\"><p style=\"flex:2 30px\">x</p></div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  const ComputedStyle& p = Style(engine, *doc, "p");
  EXPECT_FLOAT_EQ(p.flex_grow, 2.0f);
  EXPECT_FLOAT_EQ(p.flex_shrink, 1.0f);
  ASSERT_TRUE(p.flex_basis.has_value());
  EXPECT_FLOAT_EQ(p.flex_basis.value().value, 30.0f);

  // flex: 1 1 -> grow 1, shrink 1, basis 0 (omitted basis).
  auto doc2 = MakeDoc("<body><div style=\"display:flex\"><p style=\"flex:1 1\">x</p></div></body>");
  StyleEngine engine2;
  engine2.ApplyStyles(*doc2);
  const ComputedStyle& p2 = Style(engine2, *doc2, "p");
  EXPECT_FLOAT_EQ(p2.flex_grow, 1.0f);
  EXPECT_FLOAT_EQ(p2.flex_shrink, 1.0f);
  ASSERT_TRUE(p2.flex_basis.has_value());
  EXPECT_FLOAT_EQ(p2.flex_basis.value().value, 0.0f);

  // flex: 2 1 40px.
  auto doc3 =
      MakeDoc("<body><div style=\"display:flex\"><p style=\"flex:2 1 40px\">x</p></div></body>");
  StyleEngine engine3;
  engine3.ApplyStyles(*doc3);
  const ComputedStyle& p3 = Style(engine3, *doc3, "p");
  EXPECT_FLOAT_EQ(p3.flex_grow, 2.0f);
  EXPECT_FLOAT_EQ(p3.flex_shrink, 1.0f);
  ASSERT_TRUE(p3.flex_basis.has_value());
  EXPECT_FLOAT_EQ(p3.flex_basis.value().value, 40.0f);
}

TEST(StyleTest, FlexOrderAndAlignSelfParse)
{
  auto doc = MakeDoc("<body><div style=\"display:flex\"><p style=\"order:3; "
                     "align-self:flex-end\">x</p></div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  const ComputedStyle& p = Style(engine, *doc, "p");
  EXPECT_EQ(p.order, 3);
  ASSERT_TRUE(p.align_self.has_value());
  EXPECT_EQ(p.align_self.value(), AlignItems::kFlexEnd);
}

TEST(StyleTest, AlignSelfAutoIsUnset)
{
  auto doc = MakeDoc("<body><div style=\"display:flex\"><p style=\"align-self:auto\">x</p>"
                     "</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  const ComputedStyle& p = Style(engine, *doc, "p");
  EXPECT_FALSE(p.align_self.has_value());
}

TEST(StyleTest, MinMaxSizesParse)
{
  auto doc = MakeDoc("<body><div style=\"min-width:10px; max-width:50%; min-height:5px; "
                     "max-height:none\">x</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  const ComputedStyle& s = Style(engine, *doc, "div");
  ASSERT_TRUE(s.min_width.has_value());
  EXPECT_FLOAT_EQ(s.min_width.value().value, 10.0f);
  ASSERT_TRUE(s.max_width.has_value());
  EXPECT_TRUE(s.max_width.value().percent);
  EXPECT_FLOAT_EQ(s.max_width.value().value, 50.0f);
  ASSERT_TRUE(s.min_height.has_value());
  EXPECT_FLOAT_EQ(s.min_height.value().value, 5.0f);
  // max-height: none means "no constraint".
  EXPECT_FALSE(s.max_height.has_value());
}

TEST(StyleTest, AutoMarginsParseLonghandAndShorthand)
{
  auto longhand = MakeDoc("<body><div style=\"margin-left:auto; margin-top:auto\">x</div></body>");
  StyleEngine engine1;
  engine1.ApplyStyles(*longhand);
  const ComputedStyle& l = Style(engine1, *longhand, "div");
  EXPECT_TRUE(l.margin_left_auto);
  EXPECT_TRUE(l.margin_top_auto);
  EXPECT_FALSE(l.margin_right_auto);
  EXPECT_FALSE(l.margin_bottom_auto);

  auto shorthand = MakeDoc("<body><div style=\"margin:0 auto\">x</div></body>");
  StyleEngine engine2;
  engine2.ApplyStyles(*shorthand);
  const ComputedStyle& s = Style(engine2, *shorthand, "div");
  // "margin: 0 auto" -> top/bottom 0, left/right auto.
  EXPECT_FALSE(s.margin_top_auto);
  EXPECT_TRUE(s.margin_right_auto);
  EXPECT_FALSE(s.margin_bottom_auto);
  EXPECT_TRUE(s.margin_left_auto);
}

TEST(StyleTest, HeadingIsBlock)
{
  auto doc = MakeDoc("<body><h1>x</h1><h2>y</h2></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  EXPECT_EQ(Style(engine, *doc, "h1").display, Display::kBlock);
  EXPECT_EQ(Style(engine, *doc, "h2").display, Display::kBlock);
}

TEST(StyleTest, DisplayGridParses)
{
  auto doc = MakeDoc("<body><div style=\"display:grid\">x</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  EXPECT_EQ(Style(engine, *doc, "div").display, Display::kGrid);
}

TEST(StyleTest, GridTrackListParses)
{
  auto doc = MakeDoc("<body><div style=\"grid-template-columns:100px 1fr auto "
                     "repeat(2, 50px)\">x</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  const ComputedStyle& s = Style(engine, *doc, "div");
  ASSERT_EQ(s.grid_template_columns.size(), 5u);
  EXPECT_EQ(s.grid_template_columns[0].kind, GridTrack::Kind::kFixed);
  EXPECT_FLOAT_EQ(s.grid_template_columns[0].length, 100.0f);
  EXPECT_EQ(s.grid_template_columns[1].kind, GridTrack::Kind::kFr);
  EXPECT_FLOAT_EQ(s.grid_template_columns[1].fr, 1.0f);
  EXPECT_EQ(s.grid_template_columns[2].kind, GridTrack::Kind::kAuto);
  EXPECT_EQ(s.grid_template_columns[3].kind, GridTrack::Kind::kFixed);
  EXPECT_FLOAT_EQ(s.grid_template_columns[3].length, 50.0f);
  EXPECT_EQ(s.grid_template_columns[4].kind, GridTrack::Kind::kFixed);
  EXPECT_FLOAT_EQ(s.grid_template_columns[4].length, 50.0f);
}

TEST(StyleTest, GridTrackPercentAndMinMaxContent)
{
  auto doc = MakeDoc("<body><div style=\"grid-template-columns:25% min-content max-content; "
                     "grid-template-rows:30px\">x</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  const ComputedStyle& s = Style(engine, *doc, "div");
  ASSERT_EQ(s.grid_template_columns.size(), 3u);
  EXPECT_EQ(s.grid_template_columns[0].kind, GridTrack::Kind::kFixed);
  EXPECT_FLOAT_EQ(s.grid_template_columns[0].percent, 25.0f);
  EXPECT_EQ(s.grid_template_columns[1].kind, GridTrack::Kind::kMinContent);
  EXPECT_EQ(s.grid_template_columns[2].kind, GridTrack::Kind::kMaxContent);
  ASSERT_EQ(s.grid_template_rows.size(), 1u);
  EXPECT_EQ(s.grid_template_rows[0].kind, GridTrack::Kind::kFixed);
  EXPECT_FLOAT_EQ(s.grid_template_rows[0].length, 30.0f);
}

TEST(StyleTest, GridPlacementParses)
{
  auto doc = MakeDoc("<body><div style=\"grid-column:2 / span 3; grid-row:span 2\">x</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  const ComputedStyle& s = Style(engine, *doc, "div");
  EXPECT_EQ(s.grid_column_start.kind, GridPlacement::Kind::kLine);
  EXPECT_EQ(s.grid_column_start.line, 2);
  EXPECT_EQ(s.grid_column_end.kind, GridPlacement::Kind::kSpan);
  EXPECT_EQ(s.grid_column_end.span, 3);
  EXPECT_EQ(s.grid_row_start.kind, GridPlacement::Kind::kAuto);
  EXPECT_EQ(s.grid_row_end.kind, GridPlacement::Kind::kSpan);
  EXPECT_EQ(s.grid_row_end.span, 2);
}

TEST(StyleTest, GridPlacementLonghandsAndSingleLine)
{
  auto doc = MakeDoc("<body><div style=\"grid-column-start:3; grid-column-end:5; "
                     "grid-row:4\">x</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  const ComputedStyle& s = Style(engine, *doc, "div");
  EXPECT_EQ(s.grid_column_start.kind, GridPlacement::Kind::kLine);
  EXPECT_EQ(s.grid_column_start.line, 3);
  EXPECT_EQ(s.grid_column_end.kind, GridPlacement::Kind::kLine);
  EXPECT_EQ(s.grid_column_end.line, 5);
  // "grid-row: 4" sets only the start line.
  EXPECT_EQ(s.grid_row_start.kind, GridPlacement::Kind::kLine);
  EXPECT_EQ(s.grid_row_start.line, 4);
  EXPECT_EQ(s.grid_row_end.kind, GridPlacement::Kind::kAuto);
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

// The cascade rule index buckets rules by the key of their rightmost compound
// selector (id / class / tag / universal); every path must still match.
TEST(StyleTest, CascadeBucketsMatchByRightmostKey)
{
  auto doc = MakeDoc("<style>"
                     "#by-id { color: red; }"        // id bucket
                     ".by-class { color: blue; }"    // class bucket
                     ".multi.a { font-size: 30px; }" // each class bucket
                     "em { color: green; }"          // tag bucket
                     "* { margin-top: 5px; }"        // universal bucket
                     "</style>"
                     "<body>"
                     "<p id=\"by-id\">a</p>"
                     "<p class=\"by-class\">b</p>"
                     "<p class=\"multi\">c</p>"
                     "<p class=\"a\">d</p>"
                     "<p class=\"multi a\">e</p>"
                     "<em>f</em>"
                     "<b>g</b>"
                     "</body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  EXPECT_EQ(Style(engine, *doc, "#by-id").color.value(), (css::Color{255, 0, 0, 255}));
  EXPECT_EQ(Style(engine, *doc, ".by-class").color.value(), (css::Color{0, 0, 255, 255}));
  // .multi.a requires BOTH classes: neither .multi nor .a alone matches.
  EXPECT_FLOAT_EQ(Style(engine, *doc, ".multi").font_size, 16.0f);
  EXPECT_FLOAT_EQ(Style(engine, *doc, ".a").font_size, 16.0f);
  EXPECT_FLOAT_EQ(Style(engine, *doc, ".multi.a").font_size, 30.0f);
  EXPECT_EQ(Style(engine, *doc, "em").color.value(), (css::Color{0, 128, 0, 255}));
  // Universal bucket applies to every element (em/b have no UA margin).
  EXPECT_FLOAT_EQ(Style(engine, *doc, "em").margin_top.value, 5.0f);
  EXPECT_FLOAT_EQ(Style(engine, *doc, "b").margin_top.value, 5.0f);
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

TEST(StyleTest, ButtonUaDefaultAppearance)
{
  // WHATWG rendering §15.5.4: <button> is an inline-block with a native
  // (appearance:auto) look, centered text and a UA border/padding.
  auto doc = MakeDoc("<body><button>OK</button></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  const ComputedStyle& b = Style(engine, *doc, "button");
  EXPECT_EQ(b.display, Display::kInlineBlock);
  EXPECT_EQ(b.appearance, Appearance::kAuto);
  EXPECT_EQ(b.text_align, TextAlign::kCenter);
  EXPECT_FLOAT_EQ(b.padding_left.value, 6.0f);
  EXPECT_FLOAT_EQ(b.padding_top.value, 1.0f);
  EXPECT_FLOAT_EQ(b.border_top.value, 2.0f);
  // appearance:none keeps the UA box model but drops the native look.
  auto doc2 = MakeDoc("<body><button style=\"appearance: none\">OK</button></body>");
  StyleEngine engine2;
  engine2.ApplyStyles(*doc2);
  EXPECT_EQ(Style(engine2, *doc2, "button").appearance, Appearance::kNone);
}

TEST(StyleTest, AppearanceParsesNoneAutoButton)
{
  auto doc = MakeDoc("<body>"
                     "<button id=\"a\">a</button>"
                     "<div id=\"b\" style=\"appearance: auto\">b</div>"
                     "<div id=\"c\" style=\"appearance: button\">c</div>"
                     "<div id=\"d\" style=\"appearance: checkbox\">d</div>"
                     "</body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  // UA default for button; explicit auto on a plain div; forced button.
  EXPECT_EQ(Style(engine, *doc, "#a").appearance, Appearance::kAuto);
  EXPECT_EQ(Style(engine, *doc, "#b").appearance, Appearance::kAuto);
  EXPECT_EQ(Style(engine, *doc, "#c").appearance, Appearance::kButton);
  // Unsupported compat value: declaration ignored, initial value none.
  EXPECT_EQ(Style(engine, *doc, "#d").appearance, Appearance::kNone);
}

TEST(StyleTest, AuthorAppearanceOverridesUa)
{
  auto doc = MakeDoc("<body><button id=\"x\" style=\"appearance: none\">a</button>"
                     "<button id=\"y\" style=\"appearance: button\">b</button></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  EXPECT_EQ(Style(engine, *doc, "#x").appearance, Appearance::kNone);
  EXPECT_EQ(Style(engine, *doc, "#y").appearance, Appearance::kButton);
}

TEST(StyleTest, RootPseudoClassMatchesDocumentElement)
{
  auto doc = MakeDoc("<style>:root { background-color: rgb(1, 2, 3); }</style>"
                     "<body><p>x</p></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  // The <html> element (document element) matches :root; <p> does not.
  ASSERT_TRUE(Style(engine, *doc, "html").background_color.has_value());
  EXPECT_EQ(Style(engine, *doc, "html").background_color.value(), (css::Color{1, 2, 3, 255}));
  EXPECT_FALSE(Style(engine, *doc, "p").background_color.has_value());
}

TEST(StyleTest, CustomPropertiesDefineAndInherit)
{
  auto doc = MakeDoc("<style>:root { --bg: #123456; --text: rgb(10, 20, 30); }</style>"
                     "<body><p style=\"background: var(--bg); color: var(--text)\">x</p></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& root = Style(engine, *doc, "html");
  ASSERT_NE(root.custom_properties.find("--bg"), root.custom_properties.end());
  EXPECT_EQ(root.custom_properties.at("--bg"), "#123456");

  const ComputedStyle& p = Style(engine, *doc, "p");
  // Custom properties inherit from :root.
  EXPECT_EQ(p.custom_properties.at("--bg"), "#123456");
  EXPECT_EQ(p.custom_properties.at("--text"), "rgb(10, 20, 30)");
  // var() in a used property resolves to the inherited value.
  ASSERT_TRUE(p.background_color.has_value());
  EXPECT_EQ(p.background_color.value(), (css::Color{0x12, 0x34, 0x56, 255}));
  ASSERT_TRUE(p.color.has_value());
  EXPECT_EQ(p.color.value(), (css::Color{10, 20, 30, 255}));
}

TEST(StyleTest, VarFallbackUsedWhenUndefined)
{
  auto doc = MakeDoc("<body><p style=\"color: var(--missing, rgb(1, 2, 3))\">x</p></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& p = Style(engine, *doc, "p");
  ASSERT_TRUE(p.color.has_value());
  EXPECT_EQ(p.color.value(), (css::Color{1, 2, 3, 255}));
}

TEST(StyleTest, UnresolvedVarDropsDeclaration)
{
  auto doc =
      MakeDoc("<body><p style=\"color: var(--missing); background-color: red\">x</p></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& p = Style(engine, *doc, "p");
  // The unresolved var() declaration is invalid at computed-value time; the
  // unrelated declaration still applies.
  EXPECT_FALSE(p.color.has_value());
  ASSERT_TRUE(p.background_color.has_value());
  EXPECT_EQ(p.background_color.value(), (css::Color{255, 0, 0, 255}));
}

TEST(StyleTest, ElementCustomPropertyOverridesInherited)
{
  auto doc = MakeDoc("<style>:root { --c: red; } p { --c: blue; }</style>"
                     "<body><p style=\"color: var(--c)\">x</p></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& p = Style(engine, *doc, "p");
  EXPECT_EQ(p.custom_properties.at("--c"), "blue");
  ASSERT_TRUE(p.color.has_value());
  EXPECT_EQ(p.color.value(), (css::Color{0, 0, 255, 255}));
}

TEST(StyleTest, LogicalPropertiesAliases)
{
  auto doc = MakeDoc("<style>.box { inline-size: 200px; min-block-size: 40px; "
                     "margin-inline: auto; padding-block: 8px 12px; "
                     "max-inline-size: 300px; }</style>"
                     "<body><div class=\"box\">x</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& box = Style(engine, *doc, ".box");
  ASSERT_TRUE(box.width.has_value());
  EXPECT_FLOAT_EQ(box.width.value().value, 200.0f);
  ASSERT_TRUE(box.min_height.has_value());
  EXPECT_FLOAT_EQ(box.min_height.value().value, 40.0f);
  ASSERT_TRUE(box.max_width.has_value());
  EXPECT_FLOAT_EQ(box.max_width.value().value, 300.0f);
  EXPECT_TRUE(box.margin_left_auto);
  EXPECT_TRUE(box.margin_right_auto);
  EXPECT_FLOAT_EQ(box.padding_top.value, 8.0f);
  EXPECT_FLOAT_EQ(box.padding_bottom.value, 12.0f);
}

TEST(StyleTest, LogicalBlockSizeAndBorderEnd)
{
  auto doc = MakeDoc("<style>.line { block-size: 12px; border-block-end: 1px solid #000; "
                     "margin-block-end: 5px; padding-block-start: 3px; }</style>"
                     "<body><div class=\"line\">x</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& line = Style(engine, *doc, ".line");
  ASSERT_TRUE(line.height.has_value());
  EXPECT_FLOAT_EQ(line.height.value().value, 12.0f);
  EXPECT_FLOAT_EQ(line.border_bottom.value, 1.0f);
  EXPECT_EQ(line.border_style, BorderStyle::kSolid);
  EXPECT_FLOAT_EQ(line.margin_bottom.value, 5.0f);
  EXPECT_FLOAT_EQ(line.padding_top.value, 3.0f);
}

TEST(StyleTest, PlaceItemsAliasesAlignItems)
{
  auto doc = MakeDoc("<body><div style=\"display: grid; place-items: center\">x</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  EXPECT_EQ(Style(engine, *doc, "div").align_items, AlignItems::kCenter);
}

TEST(StyleTest, LogicalAndPhysicalCascadeTogether)
{
  // width and inline-size target the same physical property; the later rule
  // wins regardless of which spelling it uses.
  auto doc = MakeDoc("<style>.a { width: 100px; } .b { inline-size: 200px; }</style>"
                     "<body><div class=\"a b\">x</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& div = Style(engine, *doc, "div");
  ASSERT_TRUE(div.width.has_value());
  EXPECT_FLOAT_EQ(div.width.value().value, 200.0f);
}
TEST(StyleTest, ViewportUnitsResolveAgainstEngineViewport)
{
  // Default viewport is 800x600 (the engine's documented viewport).
  auto doc = MakeDoc("<body><div style=\"width: 50vw; height: 25vh\">x</div>"
                     "<p style=\"width: 10vmin\">y</p></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& div = Style(engine, *doc, "div");
  ASSERT_TRUE(div.width.has_value());
  EXPECT_FLOAT_EQ(div.width.value().value, 400.0f); // 50vw = 400
  ASSERT_TRUE(div.height.has_value());
  EXPECT_FLOAT_EQ(div.height.value().value, 150.0f); // 25vh = 150
  const ComputedStyle& p = Style(engine, *doc, "p");
  ASSERT_TRUE(p.width.has_value());
  EXPECT_FLOAT_EQ(p.width.value().value, 60.0f); // 10vmin = min(800,600)*0.1
}

TEST(StyleTest, CalcFunctionParses)
{
  auto doc = MakeDoc("<body><div style=\"width: calc(100% - 32px)\">x</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& div = Style(engine, *doc, "div");
  ASSERT_TRUE(div.width.has_value());
  const SizeSpec& w = div.width.value();
  EXPECT_TRUE(w.is_calc);
  EXPECT_FLOAT_EQ(w.calc.percent, 100.0f);
  EXPECT_FLOAT_EQ(w.calc.offset, -32.0f);
}

TEST(StyleTest, MinMaxClampParse)
{
  auto doc = MakeDoc("<style>#a { width: min(1200px, calc(100% - 32px)); }"
                     "#b { width: max(200px, 50%); }"
                     "#c { width: clamp(10px, 5vw, 100px); }</style>"
                     "<body><div id=\"a\">x</div><div id=\"b\">y</div>"
                     "<div id=\"c\">z</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& a = Style(engine, *doc, "#a");
  ASSERT_TRUE(a.width.has_value());
  EXPECT_TRUE(a.width.value().is_extremum);
  EXPECT_FALSE(a.width.value().extremum_is_max);
  ASSERT_EQ(a.width.value().extremum_args.size(), 2u);
  EXPECT_FLOAT_EQ(a.width.value().extremum_args[0].offset, 1200.0f);
  EXPECT_FLOAT_EQ(a.width.value().extremum_args[1].percent, 100.0f);
  EXPECT_FLOAT_EQ(a.width.value().extremum_args[1].offset, -32.0f);

  const ComputedStyle& b = Style(engine, *doc, "#b");
  ASSERT_TRUE(b.width.has_value());
  EXPECT_TRUE(b.width.value().is_extremum);
  EXPECT_TRUE(b.width.value().extremum_is_max);

  const ComputedStyle& c = Style(engine, *doc, "#c");
  ASSERT_TRUE(c.width.has_value());
  EXPECT_TRUE(c.width.value().is_clamp);
  ASSERT_EQ(c.width.value().extremum_args.size(), 3u);
}

TEST(StyleTest, ClampFontSize)
{
  // clamp(2rem, 5vw, 3.5rem): 2rem=32, 5vw=40, 3.5rem=56 at the default
  // 16px root font size and 800px viewport.
  auto doc = MakeDoc("<body><div style=\"font-size: clamp(2rem, 5vw, 3.5rem)\">x</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  EXPECT_FLOAT_EQ(Style(engine, *doc, "div").font_size, 40.0f);
}

TEST(StyleTest, VarInsideCalcResolves)
{
  auto doc = MakeDoc("<style>:root { --gutter: 20px; }</style>"
                     "<body><div style=\"width: calc(100% - var(--gutter))\">x</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& div = Style(engine, *doc, "div");
  ASSERT_TRUE(div.width.has_value());
  EXPECT_TRUE(div.width.value().is_calc);
  EXPECT_FLOAT_EQ(div.width.value().calc.percent, 100.0f);
  EXPECT_FLOAT_EQ(div.width.value().calc.offset, -20.0f);
}

TEST(StyleTest, AspectRatioParses)
{
  auto doc = MakeDoc("<body><div id=\"a\" style=\"aspect-ratio: 1\">x</div>"
                     "<div id=\"b\" style=\"aspect-ratio: 16 / 9\">y</div>"
                     "<div id=\"c\" style=\"aspect-ratio: 4/3\">z</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& a = Style(engine, *doc, "#a");
  ASSERT_TRUE(a.aspect_ratio.has_value());
  EXPECT_FLOAT_EQ(a.aspect_ratio.value(), 1.0f);
  const ComputedStyle& b = Style(engine, *doc, "#b");
  ASSERT_TRUE(b.aspect_ratio.has_value());
  EXPECT_NEAR(b.aspect_ratio.value(), 16.0f / 9.0f, 1e-5f);
  const ComputedStyle& c = Style(engine, *doc, "#c");
  ASSERT_TRUE(c.aspect_ratio.has_value());
  EXPECT_NEAR(c.aspect_ratio.value(), 4.0f / 3.0f, 1e-5f);
}

TEST(StyleTest, BorderRadiusParses)
{
  auto doc = MakeDoc("<body><div id=\"a\" style=\"border-radius: 14px\">x</div>"
                     "<div id=\"b\" style=\"border-radius: 50%\">y</div></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);

  const ComputedStyle& a = Style(engine, *doc, "#a");
  ASSERT_TRUE(a.border_radius.has_value());
  EXPECT_FLOAT_EQ(a.border_radius.value().value, 14.0f);
  EXPECT_FALSE(a.border_radius.value().percent);

  const ComputedStyle& b = Style(engine, *doc, "#b");
  ASSERT_TRUE(b.border_radius.has_value());
  EXPECT_TRUE(b.border_radius.value().percent);
  EXPECT_FLOAT_EQ(b.border_radius.value().value, 50.0f);
}
} // namespace
} // namespace neko::style
