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

TEST(StyleTest, ButtonUaDefaultAppearance) {
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

TEST(StyleTest, AppearanceParsesNoneAutoButton) {
  auto doc = MakeDoc(
      "<body>"
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

TEST(StyleTest, AuthorAppearanceOverridesUa) {
  auto doc = MakeDoc(
      "<body><button id=\"x\" style=\"appearance: none\">a</button>"
      "<button id=\"y\" style=\"appearance: button\">b</button></body>");
  StyleEngine engine;
  engine.ApplyStyles(*doc);
  EXPECT_EQ(Style(engine, *doc, "#x").appearance, Appearance::kNone);
  EXPECT_EQ(Style(engine, *doc, "#y").appearance, Appearance::kButton);
}

}  // namespace
}  // namespace neko::style
