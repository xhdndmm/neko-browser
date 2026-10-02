// Regressions for inline SVG sizing.  An <svg width="18" height="18"> icon
// (bilibili's top-nav entries, Codeberg's Octicons) is a replaced element per
// the HTML standard, but the engine measured it as an empty inline box:
//
//  * the box came out 0x0,
//  * intrinsic (min/max-content) measurement returned 0, so a flex container
//    sizing to its content dropped the icon's slot and the sibling text
//    collapsed and wrapped (bilibili's left nav items overlapped),
//  * flex and absolutely positioned boxes never took the replaced sizing
//    path at all.
//
// The size must come from CSS width/height, else the width/height
// presentational attributes, with the CSS default replaced size (300x150) as
// the last resort.  The fixtures deliberately use fixed-size boxes (or bare
// svg elements) instead of text so the assertions do not depend on font
// metrics.

#include "neko/dom/query.h"
#include "neko/html/parser.h"
#include "neko/layout/layout_tree.h"
#include "neko/style/style_engine.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>

namespace neko::layout {
namespace {

struct Page
{
  std::unique_ptr<dom::Document> doc;
  style::StyleEngine styles;
  std::unique_ptr<LayoutBox> root;
};

Page Build(std::string_view html, float viewport = 800.0f)
{
  Page page;
  page.doc = html::Parser(html).Parse();
  page.styles.ApplyStyles(*page.doc);
  layout::LayoutEngine engine(page.styles);
  page.root = engine.BuildLayoutTree(*page.doc, viewport);
  return page;
}

const LayoutBox* FindBox(const LayoutBox& box, std::string_view selector, dom::Document& doc)
{
  dom::Element* element = dom::QuerySelector(doc, selector);
  if (element == nullptr) {
    return nullptr;
  }
  if (box.element == element) {
    return &box;
  }
  for (const auto& child : box.children) {
    if (const LayoutBox* found = FindBox(*child, selector, doc)) {
      return found;
    }
  }
  for (const auto& child : box.positioned_children) {
    if (const LayoutBox* found = FindBox(*child, selector, doc)) {
      return found;
    }
  }
  for (const Line& line : box.lines) {
    for (const InlineBox& ib : line.boxes) {
      if (ib.block_box != nullptr) {
        if (const LayoutBox* found = FindBox(*ib.block_box, selector, doc)) {
          return found;
        }
      }
    }
  }
  return nullptr;
}

// The bilibili left-nav shape: a content-sized item holding icons.  Before
// the fix the item measured 0 wide and its flex children collapsed.
TEST(ReplacedSvg, AttributeSizedSvgContributesToFlexIntrinsicWidth)
{
  Page page = Build("<style>.row{display:flex;width:500px}"
                    ".it{display:flex;flex-shrink:0}</style>"
                    "<div class=row><div class=it id=a>"
                    "<svg id=s1 width=18 height=18></svg>"
                    "<svg id=s2 width=18 height=18></svg>"
                    "</div></div>");
  const LayoutBox* a = FindBox(*page.root, "#a", *page.doc);
  const LayoutBox* s1 = FindBox(*page.root, "#s1", *page.doc);
  const LayoutBox* s2 = FindBox(*page.root, "#s2", *page.doc);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(s1, nullptr);
  ASSERT_NE(s2, nullptr);
  EXPECT_FLOAT_EQ(s1->width, 18.0f) << "the width attribute sizes the replaced svg";
  EXPECT_FLOAT_EQ(s1->height, 18.0f) << "the height attribute sizes the replaced svg";
  EXPECT_FLOAT_EQ(a->width, 36.0f) << "the item must reserve both icon slots (was 0)";
  EXPECT_FLOAT_EQ(s2->x - s1->x, 18.0f) << "the icons must not overlap";
}

TEST(ReplacedSvg, CssSizedSvgGetsItsSize)
{
  Page page = Build("<style>.row{display:flex;width:500px}"
                    ".it{display:flex;flex-shrink:0}"
                    ".ci{width:18px;height:18px}</style>"
                    "<div class=row><div class=it id=a>"
                    "<svg id=s1 class=ci></svg>"
                    "<svg id=s2 class=ci></svg>"
                    "</div></div>");
  const LayoutBox* a = FindBox(*page.root, "#a", *page.doc);
  const LayoutBox* s1 = FindBox(*page.root, "#s1", *page.doc);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(s1, nullptr);
  EXPECT_FLOAT_EQ(s1->width, 18.0f) << "CSS width sizes the replaced svg";
  EXPECT_FLOAT_EQ(s1->height, 18.0f) << "CSS height sizes the replaced svg";
  EXPECT_FLOAT_EQ(a->width, 36.0f);
}

TEST(ReplacedSvg, DefaultReplacedSizeIsThreeHundredByHundredFifty)
{
  Page page = Build("<style>.row{display:flex;width:500px}</style>"
                    "<div class=row><svg id=icon></svg></div>");
  const LayoutBox* icon = FindBox(*page.root, "#icon", *page.doc);
  ASSERT_NE(icon, nullptr);
  EXPECT_FLOAT_EQ(icon->width, 300.0f) << "CSS default replaced size for <svg>";
  EXPECT_FLOAT_EQ(icon->height, 150.0f);
}

TEST(ReplacedSvg, AbsolutelyPositionedSvgUsesItsAttributes)
{
  Page page = Build("<div style=\"position:relative;width:500px;height:100px\">"
                    "<svg id=icon style=\"position:absolute\" width=24 height=16></svg>"
                    "</div>");
  const LayoutBox* icon = FindBox(*page.root, "#icon", *page.doc);
  ASSERT_NE(icon, nullptr);
  EXPECT_FLOAT_EQ(icon->width, 24.0f);
  EXPECT_FLOAT_EQ(icon->height, 16.0f);
}

} // namespace
} // namespace neko::layout
