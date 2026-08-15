#include "neko/dom/query.h"
#include "neko/graphics/font_registry.h"
#include "neko/graphics/system_fonts.h"
#include "neko/html/parser.h"
#include "neko/image/image.h"
#include "neko/layout/layout_tree.h"
#include "neko/style/style_engine.h"

#include <gtest/gtest.h>
#include <memory>
#include <optional>
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
  // Depth-first search for the box whose element matches.
  if (box.element == element) {
    return &box;
  }
  for (const auto& child : box.children) {
    if (const LayoutBox* found = FindBox(*child, selector, doc)) {
      return found;
    }
  }
  return nullptr;
}

// Finds the first inline-block box within |box| (via line box block_boxes)
// whose element matches |element|; surfaces the containing InlineBox too.
const LayoutBox*
FindInlineBlock(const LayoutBox& box, const dom::Element* element, const InlineBox*& holder)
{
  for (const Line& line : box.lines) {
    for (const InlineBox& ib : line.boxes) {
      if (ib.block_box != nullptr && ib.block_box->element == element) {
        holder = &ib;
        return ib.block_box.get();
      }
      if (ib.block_box != nullptr) {
        if (const LayoutBox* found = FindInlineBlock(*ib.block_box, element, holder)) {
          return found;
        }
      }
    }
  }
  for (const auto& child : box.children) {
    if (const LayoutBox* found = FindInlineBlock(*child, element, holder)) {
      return found;
    }
  }
  return nullptr;
}

TEST(LayoutTest, BlockFillsContainingBlock)
{
  Page page = Build("<body><div>x</div></body>");
  ASSERT_NE(page.root, nullptr);

  const LayoutBox* body = FindBox(*page.root, "body", *page.doc);
  ASSERT_NE(body, nullptr);
  // body margin 8px each side; border box fills 800 - 16.
  EXPECT_FLOAT_EQ(body->x, 8.0f);
  EXPECT_FLOAT_EQ(body->width, 784.0f);

  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  // div fills the body content box (starts at body content x = 8).
  EXPECT_FLOAT_EQ(div->x, 8.0f);
  EXPECT_FLOAT_EQ(div->width, 784.0f);
}

TEST(LayoutTest, VerticalStacking)
{
  Page page = Build("<body><div>one</div><div>two</div></body>");
  const LayoutBox* body = FindBox(*page.root, "body", *page.doc);
  ASSERT_NE(body, nullptr);
  ASSERT_EQ(body->children.size(), 2u);
  const LayoutBox* first = body->children[0].get();
  const LayoutBox* second = body->children[1].get();
  EXPECT_FLOAT_EQ(second->y, first->y + first->height);
  // body border box bottom coincides with the last child's bottom.
  EXPECT_FLOAT_EQ(body->y + body->height, second->y + second->height);
}

TEST(LayoutTest, BoxModelWithPaddingBorderMargin)
{
  Page page = Build("<body><div style=\"width: 200px; padding: 10px; border: 2px solid; "
                    "margin: 5px\">x</div></body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  EXPECT_FLOAT_EQ(div->width, 224.0f); // 200 + 2*10 + 2*2
  EXPECT_FLOAT_EQ(div->content_width(), 200.0f);
  EXPECT_FLOAT_EQ(div->margin_top, 5.0f);
  EXPECT_FLOAT_EQ(div->border_top, 2.0f);
  EXPECT_FLOAT_EQ(div->padding_top, 10.0f);
}

TEST(LayoutTest, ExplicitHeight)
{
  Page page = Build("<body><div style=\"height: 100px\">x</div></body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  EXPECT_FLOAT_EQ(div->height, 100.0f);
}

TEST(LayoutTest, PercentWidth)
{
  Page page = Build("<body><div style=\"width: 50%\">x</div></body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  // 50% of body content width (784).
  EXPECT_FLOAT_EQ(div->width, 392.0f);
}

TEST(LayoutTest, CalcWidthResolves)
{
  // calc(100% - 32px) against the body content width (784).
  Page page = Build("<body><div style=\"width: calc(100% - 32px)\">x</div></body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  EXPECT_FLOAT_EQ(div->width, 752.0f);
}

TEST(LayoutTest, MinWidthClampsToSmaller)
{
  // min(200px, calc(100% - 40px)): the calc term is 744, so 200 wins.
  Page page = Build("<body><div style=\"width: min(200px, calc(100% - 40px))\">x</div></body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  EXPECT_FLOAT_EQ(div->width, 200.0f);
}

TEST(LayoutTest, MinWidthClampsToLarger)
{
  // min(1200px, calc(100% - 32px)): the calc term (752) is smaller, so it wins.
  Page page = Build("<body><div style=\"width: min(1200px, calc(100% - 32px))\">x</div></body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  EXPECT_FLOAT_EQ(div->width, 752.0f);
}

TEST(LayoutTest, ClampWidth)
{
  // clamp(100px, 50%, 500px): 50% of 784 = 392, within [100, 500].
  Page page = Build("<body><div style=\"width: clamp(100px, 50%, 500px)\">x</div></body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  EXPECT_FLOAT_EQ(div->width, 392.0f);
}

TEST(LayoutTest, AspectRatioDerivesHeight)
{
  // Definite width + auto height + aspect-ratio: 1 -> a square.
  Page page = Build("<body><div style=\"width: 150px; aspect-ratio: 1\">x</div></body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  EXPECT_FLOAT_EQ(div->width, 150.0f);
  EXPECT_FLOAT_EQ(div->height, 150.0f);
}

TEST(LayoutTest, AspectRatioRatioNonSquare)
{
  // aspect-ratio: 16/9 with a definite 160px width -> 90px height.
  Page page = Build("<body><div style=\"width: 160px; aspect-ratio: 16/9\">x</div></body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  EXPECT_FLOAT_EQ(div->height, 90.0f);
}

TEST(LayoutTest, ExplicitHeightBeatsAspectRatio)
{
  // An explicit height wins over aspect-ratio.
  Page page = Build("<body><div style=\"width: 150px; height: 40px; aspect-ratio: 1\">x</div>"
                    "</body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  EXPECT_FLOAT_EQ(div->height, 40.0f);
}

TEST(LayoutTest, InlineTextCreatesLines)
{
  Page page = Build("<body><p>hello world</p></body>");
  const LayoutBox* p = FindBox(*page.root, "p", *page.doc);
  ASSERT_NE(p, nullptr);
  ASSERT_FALSE(p->lines.empty());
  ASSERT_GE(p->lines[0].runs.size(), 1u);
  EXPECT_EQ(p->lines[0].runs[0].text, "hello");
  // Default font size 16 -> line height 19.2.
  EXPECT_FLOAT_EQ(p->lines[0].height, 19.2f);
}

TEST(LayoutTest, TextWrapsAtWordBoundary)
{
  // 100px content width fits one 16px-wide word per line at most.
  Page page = Build("<body><div style=\"width: 100px\">one two three four</div></body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  ASSERT_GE(div->lines.size(), 2u); // wrapped into multiple lines
  EXPECT_LE(div->lines[0].runs.size(), 2u);
}

TEST(LayoutTest, WrappedHeadingLinesDoNotOverlap)
{
  // h1 is 2em = 32px; its line height must scale with the font size so that
  // wrapped lines stack without overlapping. (Regression: line-height was a
  // fixed 19.2px, so the 32px glyphs overlapped vertically.)
  Page page = Build("<body><h1>one two three</h1></body>", 120.0f);
  const LayoutBox* h1 = FindBox(*page.root, "h1", *page.doc);
  ASSERT_NE(h1, nullptr);
  ASSERT_GE(h1->lines.size(), 2u);
  EXPECT_FLOAT_EQ(h1->lines[0].height, 38.4f);
  const float first_bottom = h1->lines[0].runs[0].y + h1->lines[0].runs[0].font_size;
  EXPECT_GE(h1->lines[1].runs[0].y, first_bottom);
}

TEST(LayoutTest, DisplayNoneSkipped)
{
  Page page = Build("<body><div>visible</div><div style=\"display:none\">hidden</div></body>");
  const LayoutBox* body = FindBox(*page.root, "body", *page.doc);
  ASSERT_NE(body, nullptr);
  ASSERT_EQ(body->children.size(), 1u);
  EXPECT_EQ(body->children[0]->element->tag_name(), "div");
}

TEST(LayoutTest, InlineElementStyleAppliesToText)
{
  Page page = Build("<body><p>a <span style=\"color: #ff0000\">b</span> c</p></body>");
  const LayoutBox* p = FindBox(*page.root, "p", *page.doc);
  ASSERT_NE(p, nullptr);
  ASSERT_FALSE(p->lines.empty());
  // Find the red run.
  bool found_red = false;
  for (const Line& line : p->lines) {
    for (const TextRun& run : line.runs) {
      if (run.text == "b" && run.color.r == 255 && run.color.g == 0) {
        found_red = true;
      }
    }
  }
  EXPECT_TRUE(found_red);
}

TEST(LayoutTest, RelativePositionShift)
{
  Page page = Build("<body><div style=\"position: relative; top: 10px; left: 5px\">x</div></body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  EXPECT_FLOAT_EQ(div->y, 8.0f + 10.0f); // body content y (8) + offset
}

TEST(LayoutTest, BrForcesLineBreak)
{
  Page page = Build("<body><p>one<br>two</p></body>");
  const LayoutBox* p = FindBox(*page.root, "p", *page.doc);
  ASSERT_NE(p, nullptr);
  ASSERT_GE(p->lines.size(), 2u);
  EXPECT_EQ(p->lines[0].runs[0].text, "one");
  EXPECT_EQ(p->lines[1].runs[0].text, "two");
  // The second line sits below the first.
  EXPECT_GT(p->lines[1].runs[0].y, p->lines[0].runs[0].y);
}

TEST(LayoutTest, ConsecutiveBrProduceEmptyLines)
{
  Page page = Build("<body><p>a<br><br>b</p></body>");
  const LayoutBox* p = FindBox(*page.root, "p", *page.doc);
  ASSERT_NE(p, nullptr);
  // a | (empty) | b: three lines, the middle one has no runs.
  ASSERT_GE(p->lines.size(), 3u);
  EXPECT_EQ(p->lines[0].runs.size(), 1u);
  EXPECT_EQ(p->lines[1].runs.size(), 0u);
  EXPECT_GT(p->lines[1].height, 0.0f);
  EXPECT_EQ(p->lines[2].runs[0].text, "b");
  // Total height includes the empty line.
  EXPECT_GT(p->height, p->lines[0].height + p->lines[1].height);
}

TEST(LayoutTest, BrInsideInlineElement)
{
  // <br> nested in an inline element still breaks the line.
  Page page = Build("<body><p>a<span>b<br>c</span>d</p></body>");
  const LayoutBox* p = FindBox(*page.root, "p", *page.doc);
  ASSERT_NE(p, nullptr);
  ASSERT_GE(p->lines.size(), 2u);
  EXPECT_EQ(p->lines[0].runs.size(), 2u); // "a" + "b"
  EXPECT_EQ(p->lines[1].runs.size(), 2u); // "c" + "d"
}

TEST(LayoutTest, BoldAndItalicReachTextRuns)
{
  Page page = Build("<body><p><b>bold</b> <i>italic</i> <em>em</em> plain</p></body>");
  const LayoutBox* p = FindBox(*page.root, "p", *page.doc);
  ASSERT_NE(p, nullptr);
  ASSERT_GE(p->lines.size(), 1u);
  // Runs: "bold" (700), "italic" (italic), "em" (italic), "plain" (400).
  bool saw_bold = false;
  bool saw_italic = false;
  bool saw_plain = false;
  for (const TextRun& run : p->lines[0].runs) {
    if (run.text == "bold") {
      EXPECT_EQ(run.font_weight, 700);
      saw_bold = true;
    } else if (run.text == "italic" || run.text == "em") {
      EXPECT_TRUE(run.font_italic);
      saw_italic = true;
    } else if (run.text == "plain") {
      EXPECT_EQ(run.font_weight, 400);
      EXPECT_FALSE(run.font_italic);
      saw_plain = true;
    }
  }
  EXPECT_TRUE(saw_bold);
  EXPECT_TRUE(saw_italic);
  EXPECT_TRUE(saw_plain);
}

TEST(LayoutTest, ImageBoxSizing)
{
  // Fake provider with a 100x50 intrinsic image.
  struct FakeProvider : public layout::ImageProvider
  {
    image::Image img;
    FakeProvider()
    {
      img.width = 100;
      img.height = 50;
    }
    const image::Image* Find(const dom::Element&) const override
    {
      return &img;
    }
  };

  const auto build = [](const layout::ImageProvider* provider, std::string_view html) {
    auto doc = html::Parser(html).Parse();
    style::StyleEngine styles;
    styles.ApplyStyles(*doc);
    layout::LayoutEngine engine(styles, nullptr, provider);
    std::unique_ptr<LayoutBox> root = engine.BuildLayoutTree(*doc, 800);
    return std::make_pair(std::move(doc), std::move(root));
  };
  const auto img_box = [](const std::unique_ptr<dom::Document>& doc,
                          const std::unique_ptr<LayoutBox>& root) -> const InlineBox* {
    // The <img> is an atomic inline box in the body's first line.
    const LayoutBox* body = FindBox(*root, "body", *doc);
    if (body == nullptr || body->lines.empty() || body->lines[0].boxes.empty()) {
      return nullptr;
    }
    return &body->lines[0].boxes[0];
  };

  FakeProvider provider;

  // Intrinsic size used when nothing is specified.
  {
    auto [doc, root] = build(&provider, "<body><img></body>");
    const InlineBox* box = img_box(doc, root);
    ASSERT_NE(box, nullptr);
    EXPECT_FLOAT_EQ(box->width, 100.0f);
    EXPECT_FLOAT_EQ(box->height, 50.0f);
  }

  // Explicit width keeps the aspect ratio.
  {
    auto [doc, root] = build(&provider, "<body><img style=\"width: 200px\"></body>");
    const InlineBox* box = img_box(doc, root);
    ASSERT_NE(box, nullptr);
    EXPECT_FLOAT_EQ(box->width, 200.0f);
    EXPECT_FLOAT_EQ(box->height, 100.0f);
  }

  // Both dimensions specified win.
  {
    auto [doc, root] = build(&provider, "<body><img style=\"width: 40px; height: 30px\"></body>");
    const InlineBox* box = img_box(doc, root);
    ASSERT_NE(box, nullptr);
    EXPECT_FLOAT_EQ(box->width, 40.0f);
    EXPECT_FLOAT_EQ(box->height, 30.0f);
  }

  // Presentational width attribute acts as a fallback.
  {
    auto [doc, root] = build(&provider, "<body><img width=\"20\"></body>");
    const InlineBox* box = img_box(doc, root);
    ASSERT_NE(box, nullptr);
    EXPECT_FLOAT_EQ(box->width, 20.0f);
    EXPECT_FLOAT_EQ(box->height, 10.0f);
  }

  // No provider: image boxes collapse to zero.
  {
    auto [doc, root] = build(nullptr, "<body><img></body>");
    const InlineBox* box = img_box(doc, root);
    ASSERT_NE(box, nullptr);
    EXPECT_FLOAT_EQ(box->width, 0.0f);
    EXPECT_FLOAT_EQ(box->height, 0.0f);
  }
}

TEST(LayoutTest, InlineImageFlowsWithText)
{
  // <img> between text runs must share a line (no forced break).
  struct FakeProvider : public layout::ImageProvider
  {
    image::Image img;
    FakeProvider()
    {
      img.width = 20;
      img.height = 20;
    }
    const image::Image* Find(const dom::Element&) const override
    {
      return &img;
    }
  };
  FakeProvider provider;
  auto doc = html::Parser("<body><p>a<img>b</p></body>").Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles, nullptr, &provider);
  auto root = engine.BuildLayoutTree(*doc, 800);
  const LayoutBox* p = FindBox(*root, "p", *doc);
  ASSERT_NE(p, nullptr);
  ASSERT_GE(p->lines.size(), 1u);
  ASSERT_EQ(p->lines[0].boxes.size(), 1u);
  // One line holds "a", the image, then "b" in that order.
  ASSERT_GE(p->lines[0].runs.size(), 2u);
  EXPECT_EQ(p->lines[0].runs[0].text, "a");
  EXPECT_EQ(p->lines[0].runs.back().text, "b");
  const InlineBox& img = p->lines[0].boxes[0];
  EXPECT_FLOAT_EQ(img.width, 20.0f);
  EXPECT_FLOAT_EQ(img.height, 20.0f);
  // The image sits horizontally after "a" and before "b".
  EXPECT_GT(img.x, p->lines[0].runs[0].x);
  EXPECT_LT(img.x, p->lines[0].runs.back().x);
}

TEST(LayoutTest, ImageInTableCellTranslatedToSlot)
{
  // Regression: an <img> inside a table cell is laid out at a local origin and
  // then translated to its grid slot; the InlineBox must move with the cell
  // (it used to stay at the local y, overlapping earlier content).
  struct FakeProvider : public layout::ImageProvider
  {
    image::Image img;
    FakeProvider()
    {
      img.width = 20;
      img.height = 20;
    }
    const image::Image* Find(const dom::Element&) const override
    {
      return &img;
    }
  };
  FakeProvider provider;
  auto doc = html::Parser("<body><table><tr><td><img></td></tr></table></body>").Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles, nullptr, &provider);
  auto root = engine.BuildLayoutTree(*doc, 800);

  const LayoutBox* td = FindBox(*root, "td", *doc);
  ASSERT_NE(td, nullptr);
  ASSERT_GE(td->lines.size(), 1u);
  ASSERT_GE(td->lines[0].boxes.size(), 1u);
  const InlineBox& img = td->lines[0].boxes[0];
  // The image sits at the cell's content origin (body margin offset), not the
  // local (0,0) it was measured at.
  EXPECT_FLOAT_EQ(img.y, td->content_y());
}

TEST(LayoutTest, AbsolutePositioningAgainstRelativeAncestor)
{
  // An absolutely positioned child is placed against its nearest positioned
  // ancestor's padding box using bottom/right offsets.
  auto doc = html::Parser("<body><div style=\"position:relative;width:200px;height:100px\">"
                          "<div style=\"position:absolute;bottom:10px;right:15px\">x</div>"
                          "</div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);

  const LayoutBox* rel = FindBox(*root, "div", *doc);
  ASSERT_NE(rel, nullptr);
  ASSERT_EQ(rel->positioned_children.size(), 1u);
  const LayoutBox* abs = rel->positioned_children[0].get();
  EXPECT_FLOAT_EQ(abs->x, rel->content_x() + rel->content_width() - 15.0f - abs->width);
  EXPECT_FLOAT_EQ(abs->y, rel->content_y() + rel->content_height() - 10.0f - abs->height);
}

TEST(LayoutTest, AbsoluteUsesNearestPositionedAncestor)
{
  // The containing block is the nearest positioned ancestor, skipping a
  // static intermediate div.
  auto doc = html::Parser("<body><div style=\"position:relative;width:300px\">"
                          "<div><div style=\"position:absolute;left:20px;top:5px\">x</div></div>"
                          "</div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);

  const LayoutBox* rel = FindBox(*root, "div", *doc);
  ASSERT_NE(rel, nullptr);
  // The static intermediate div has one in-flow child which itself holds the
  // absolutely positioned box.
  ASSERT_EQ(rel->children.size(), 1u);
  const LayoutBox* mid = rel->children[0].get();
  ASSERT_EQ(mid->positioned_children.size(), 1u);
  const LayoutBox* abs = mid->positioned_children[0].get();
  EXPECT_FLOAT_EQ(abs->x, rel->content_x() + 20.0f);
  EXPECT_FLOAT_EQ(abs->y, rel->content_y() + 5.0f);
}

TEST(LayoutTest, LeadingWhitespaceDoesNotShiftInlineImage)
{
  // HTML source indentation (newline + spaces) before an <img> must collapse
  // to nothing, not become an inline space that shifts the image right.
  struct FakeProvider : public layout::ImageProvider
  {
    image::Image img;
    FakeProvider()
    {
      img.width = 20;
      img.height = 20;
    }
    const image::Image* Find(const dom::Element&) const override
    {
      return &img;
    }
  };
  FakeProvider provider;
  auto doc = html::Parser("<body><div>\n    <img>\n</div></body>").Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles, nullptr, &provider);
  auto root = engine.BuildLayoutTree(*doc, 800);

  const LayoutBox* div = FindBox(*root, "div", *doc);
  ASSERT_NE(div, nullptr);
  ASSERT_GE(div->lines.size(), 1u);
  ASSERT_GE(div->lines[0].boxes.size(), 1u);
  const InlineBox& img = div->lines[0].boxes[0];
  EXPECT_FLOAT_EQ(img.x, div->content_x());
}

TEST(LayoutTest, AbsoluteShrinkToFitExcludesInset)
{
  // A left inset constrains the shrink-to-fit available width (CSS2.2 §10.3.7
  // case 3): the box must not overflow the containing block's right edge.
  auto doc = html::Parser("<body><div style=\"position:relative;width:300px\">"
                          "<div style=\"position:absolute;left:10px\">"
                          "some quite long text that would otherwise overflow</div>"
                          "</div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);

  const LayoutBox* rel = FindBox(*root, "div", *doc);
  ASSERT_NE(rel, nullptr);
  ASSERT_EQ(rel->positioned_children.size(), 1u);
  const LayoutBox* abs = rel->positioned_children[0].get();
  // Right edge stays within the containing block's content box.
  EXPECT_LE(abs->x + abs->width, rel->content_x() + rel->content_width() + 0.5f);
}

TEST(LayoutTest, AbsoluteShrinkToFitCollapsesWhitespace)
{
  // Source indentation inside the element must not inflate shrink-to-fit.
  auto doc = html::Parser("<body><div style=\"position:relative;width:300px\">"
                          "<div style=\"position:absolute;left:0px\">\n      abc\n    </div>"
                          "</div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);

  const LayoutBox* rel = FindBox(*root, "div", *doc);
  ASSERT_NE(rel, nullptr);
  ASSERT_EQ(rel->positioned_children.size(), 1u);
  const LayoutBox* abs = rel->positioned_children[0].get();
  // "abc" in the monospace fallback is 3 * 16 = 48px; indentation adds nothing.
  EXPECT_FLOAT_EQ(abs->width, 48.0f);
}

TEST(LayoutTest, NestedBlockHeightInherits)
{
  Page page = Build("<body><div><div>text</div></div></body>");
  const LayoutBox* outer = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(outer, nullptr);
  ASSERT_EQ(outer->children.size(), 1u);
  const LayoutBox* inner = outer->children[0].get();
  // outer height = inner height (both content-based).
  EXPECT_FLOAT_EQ(outer->height, inner->height);
}

TEST(LayoutTest, RealFontAdvancesWhenFontProvided)
{
  // Skip unless a system font exists (any desktop has one).
  if (graphics::FindSystemFonts(graphics::GenericFamily::kSansSerif).empty()) {
    GTEST_SKIP() << "no system sans-serif font available";
  }
  graphics::FontRegistry registry;

  // Same HTML, laid out with and without a font registry.
  struct Built
  {
    std::unique_ptr<dom::Document> doc;
    std::unique_ptr<LayoutBox> root;
  };
  const auto build = [&](const graphics::FontRegistry* fonts) -> Built {
    auto doc = html::Parser("<body><p>hello</p></body>").Parse();
    style::StyleEngine styles;
    styles.ApplyStyles(*doc);
    layout::LayoutEngine engine(styles, fonts);
    std::unique_ptr<LayoutBox> root = engine.BuildLayoutTree(*doc, 800);
    return Built{std::move(doc), std::move(root)};
  };
  Built monospace = build(nullptr);
  Built real = build(&registry);

  const auto hello_width = [](const Built& built) {
    const LayoutBox* p_box = FindBox(*built.root, "p", *built.doc);
    EXPECT_NE(p_box, nullptr);
    return p_box->lines[0].runs[0].width;
  };
  const float monospace_width = hello_width(monospace);
  const float real_width = hello_width(real);

  // Monospace fallback: 5 chars * 16px.
  EXPECT_FLOAT_EQ(monospace_width, 80.0f);
  // Real advances are nonzero and narrower than the monospace model for a
  // proportional font.
  EXPECT_GT(real_width, 0.0f);
  EXPECT_LT(real_width, 80.0f);
}

TEST(LayoutTest, TableCellsAreSideBySide)
{
  Page page = Build("<body><table><tr><td>A</td><td>B</td></tr>"
                    "<tr><td>C</td><td>D</td></tr></table></body>");
  const LayoutBox* table = FindBox(*page.root, "table", *page.doc);
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->children.size(), 2u); // two rows

  const LayoutBox* row0 = table->children[0].get();
  const LayoutBox* row1 = table->children[1].get();
  ASSERT_EQ(row0->children.size(), 2u);
  ASSERT_EQ(row1->children.size(), 2u);

  // Cells in a row share the same top and sit side by side.
  EXPECT_FLOAT_EQ(row0->children[0]->y, row0->children[1]->y);
  EXPECT_FLOAT_EQ(row0->children[0]->x, 8.0f); // body content x
  EXPECT_GT(row0->children[1]->x, row0->children[0]->x);
  // The second row is below the first.
  EXPECT_GT(row1->y, row0->y);
}

TEST(LayoutTest, TableExplicitCellWidthFixesColumn)
{
  Page page = Build("<body><table style=\"width: 400px\"><tr>"
                    "<td style=\"width: 100px\">A</td><td>B</td></tr></table></body>");
  const LayoutBox* table = FindBox(*page.root, "table", *page.doc);
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->children.size(), 1u);

  const LayoutBox* row = table->children[0].get();
  ASSERT_EQ(row->children.size(), 2u);
  // First column is fixed at 100px; the second takes the remaining 300px.
  EXPECT_FLOAT_EQ(row->children[0]->width, 100.0f);
  EXPECT_FLOAT_EQ(row->children[1]->width, 300.0f);
}

TEST(LayoutTest, TableColspanSpansColumns)
{
  Page page = Build("<body><table style=\"width: 400px\">"
                    "<tr><td colspan=\"2\">wide</td></tr>"
                    "<tr><td style=\"width: 100px\">A</td><td>B</td></tr></table></body>");
  const LayoutBox* table = FindBox(*page.root, "table", *page.doc);
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->children.size(), 2u);

  const LayoutBox* row0 = table->children[0].get();
  const LayoutBox* row1 = table->children[1].get();
  ASSERT_EQ(row0->children.size(), 1u); // one spanning cell
  ASSERT_EQ(row1->children.size(), 2u);
  // The colspan cell spans the full table width (both columns).
  EXPECT_FLOAT_EQ(row0->children[0]->width, 400.0f);
  // Second row: fixed 100px column + 300px remainder.
  EXPECT_FLOAT_EQ(row1->children[0]->width, 100.0f);
  EXPECT_FLOAT_EQ(row1->children[1]->width, 300.0f);
}

TEST(LayoutTest, TableRowspanZeroSpansToEnd)
{
  // rowspan="0" means "span the remaining rows of the row group" (WHATWG HTML
  // tables.html); with a single flattened group that is the rest of the table.
  Page page = Build("<body><table style=\"width: 400px\">"
                    "<tr><td rowspan=\"0\">X</td><td>A</td></tr>"
                    "<tr><td>B</td></tr></table></body>");
  const LayoutBox* table = FindBox(*page.root, "table", *page.doc);
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->children.size(), 2u);

  const LayoutBox* row0 = table->children[0].get();
  const LayoutBox* row1 = table->children[1].get();
  ASSERT_EQ(row0->children.size(), 2u); // X (rowspan to end) + A
  ASSERT_EQ(row1->children.size(), 1u); // B (X continues)
  // X spans both rows.
  EXPECT_FLOAT_EQ(row0->children[0]->height, row0->height + row1->height);
}

TEST(LayoutTest, TableRowspanZeroStopsAtRowGroupEnd)
{
  // rowspan="0" spans only to the end of its own row group.  The <thead> cell
  // must NOT grow through the <tbody> rows.
  Page page = Build("<body><table style=\"width: 400px\">"
                    "<thead><tr><td rowspan=\"0\">H</td></tr></thead>"
                    "<tbody><tr><td>A</td></tr><tr><td>B</td></tr></tbody>"
                    "</table></body>");
  const LayoutBox* table = FindBox(*page.root, "table", *page.doc);
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->children.size(), 3u); // thead row + 2 tbody rows

  const LayoutBox* row0 = table->children[0].get(); // thead
  const LayoutBox* row1 = table->children[1].get(); // tbody
  const LayoutBox* row2 = table->children[2].get(); // tbody
  ASSERT_EQ(row0->children.size(), 1u);             // H
  ASSERT_EQ(row1->children.size(), 1u);             // A
  ASSERT_EQ(row2->children.size(), 1u);             // B
  // H spans only the thead row (its own group), not the whole table.
  EXPECT_FLOAT_EQ(row0->children[0]->height, row0->height);
}

TEST(LayoutTest, TableRowspanZeroImplicitGroup)
{
  // Consecutive anonymous <tr> children form one implicit row group, so
  // rowspan="0" spans all of them.
  Page page = Build("<body><table style=\"width: 400px\">"
                    "<tr><td rowspan=\"0\">X</td></tr>"
                    "<tr><td>A</td></tr>"
                    "<tr><td>B</td></tr></table></body>");
  const LayoutBox* table = FindBox(*page.root, "table", *page.doc);
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->children.size(), 3u);

  const LayoutBox* row0 = table->children[0].get();
  const LayoutBox* row1 = table->children[1].get();
  const LayoutBox* row2 = table->children[2].get();
  ASSERT_EQ(row0->children.size(), 1u); // X
  // X occupies the first column across all three rows; A/B land in column 1.
  EXPECT_FLOAT_EQ(row0->children[0]->height, row0->height + row1->height + row2->height);
  ASSERT_EQ(row1->children.size(), 1u);
  ASSERT_EQ(row2->children.size(), 1u);
}

TEST(LayoutTest, TableColspanClampsAboveThousand)
{
  // colspan above 1000 clamps to 1000 (WHATWG tables.html); the single
  // spanning cell then covers the whole 400px table.
  Page page = Build("<body><table style=\"width: 400px\">"
                    "<tr><td colspan=\"1001\">wide</td></tr></table></body>");
  const LayoutBox* table = FindBox(*page.root, "table", *page.doc);
  ASSERT_NE(table, nullptr);
  const LayoutBox* row = table->children[0].get();
  ASSERT_EQ(row->children.size(), 1u);
  // 1000 columns of 0.4px accumulate float error, so allow a small tolerance.
  EXPECT_NEAR(row->children[0]->width, 400.0f, 0.05f);
}

TEST(LayoutTest, TableColspanIgnoresTrailingText)
{
  // colspan="2abc" parses as 2 (trailing non-digits are ignored).
  Page page = Build("<body><table style=\"width: 400px\">"
                    "<tr><td colspan=\"2abc\">wide</td></tr>"
                    "<tr><td style=\"width: 100px\">A</td><td>B</td></tr></table></body>");
  const LayoutBox* table = FindBox(*page.root, "table", *page.doc);
  ASSERT_NE(table, nullptr);
  const LayoutBox* row0 = table->children[0].get();
  const LayoutBox* row1 = table->children[1].get();
  ASSERT_EQ(row0->children.size(), 1u);
  ASSERT_EQ(row1->children.size(), 2u);
  // The spanning cell covers both columns.
  EXPECT_FLOAT_EQ(row0->children[0]->width, 400.0f);
  EXPECT_FLOAT_EQ(row1->children[0]->width, 100.0f);
  EXPECT_FLOAT_EQ(row1->children[1]->width, 300.0f);
}

TEST(LayoutTest, TableInvalidSpanFallsBackToOne)
{
  // A leading non-digit (colspan="x2") is an invalid non-negative integer, so
  // the span falls back to 1: the cell covers only the first column.
  Page page = Build("<body><table style=\"width: 400px\">"
                    "<tr><td colspan=\"x2\">narrow</td></tr>"
                    "<tr><td style=\"width: 100px\">A</td><td>B</td></tr></table></body>");
  const LayoutBox* table = FindBox(*page.root, "table", *page.doc);
  ASSERT_NE(table, nullptr);
  const LayoutBox* row0 = table->children[0].get();
  ASSERT_EQ(row0->children.size(), 1u);
  EXPECT_FLOAT_EQ(row0->children[0]->width, 100.0f);
}

TEST(LayoutTest, TableRowspanSpansRows)
{
  Page page = Build("<body><table style=\"width: 400px\">"
                    "<tr><td rowspan=\"2\">tall</td><td>R</td></tr>"
                    "<tr><td>S</td></tr></table></body>");
  const LayoutBox* table = FindBox(*page.root, "table", *page.doc);
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->children.size(), 2u);

  const LayoutBox* row0 = table->children[0].get();
  const LayoutBox* row1 = table->children[1].get();
  ASSERT_EQ(row0->children.size(), 2u); // rowspan cell + R
  ASSERT_EQ(row1->children.size(), 1u); // S (rowspan cell continues)
  // The rowspan cell's height spans both rows.
  EXPECT_FLOAT_EQ(row0->children[0]->height, row0->height + row1->height);
  // S sits in the second row, below the first.
  EXPECT_GT(row1->children[0]->y, row0->y);
}

TEST(LayoutTest, InlineBlockExplicitSizeIsAtomicBox)
{
  // An inline-block with explicit size is an atomic inline box on its line.
  auto doc = html::Parser("<body><p>a<span style=\"display:inline-block;width:50px;height:30px\">b"
                          "</span>c</p></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);
  const LayoutBox* p = FindBox(*root, "p", *doc);
  ASSERT_NE(p, nullptr);
  ASSERT_GE(p->lines.size(), 1u);
  ASSERT_EQ(p->lines[0].boxes.size(), 1u);
  dom::Element* el = dom::QuerySelector(*doc, "span");
  ASSERT_NE(el, nullptr);
  const InlineBox* holder = nullptr;
  const LayoutBox* block = FindInlineBlock(*root, el, holder);
  ASSERT_NE(block, nullptr);
  ASSERT_NE(holder, nullptr);
  EXPECT_FLOAT_EQ(holder->width, 50.0f);
  EXPECT_FLOAT_EQ(holder->height, 30.0f);
  EXPECT_FLOAT_EQ(block->width, 50.0f);
  EXPECT_FLOAT_EQ(block->height, 30.0f);
  // It flows between the text runs "a" and "c" on the same line.
  ASSERT_GE(p->lines[0].runs.size(), 2u);
  EXPECT_EQ(p->lines[0].runs[0].text, "a");
  EXPECT_EQ(p->lines[0].runs.back().text, "c");
}

TEST(LayoutTest, InlineBlockShrinkToFitWidth)
{
  // Auto width: inline-block wraps its content (preferred/max-content width).
  auto doc = html::Parser("<body><div style=\"width:600px\">"
                          "<span style=\"display:inline-block\">hello world</span></div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);
  dom::Element* el = dom::QuerySelector(*doc, "span");
  ASSERT_NE(el, nullptr);
  const InlineBox* holder = nullptr;
  const LayoutBox* block = FindInlineBlock(*root, el, holder);
  ASSERT_NE(block, nullptr);
  EXPECT_GT(block->width, 0.0f);
  EXPECT_LT(block->width, 600.0f); // does not fill the container
}

TEST(LayoutTest, InlineBlockHoldsBlockChildren)
{
  // The inline-block's content is a block formatting context; a block child
  // stacks vertically inside it (an outer <div> is used: a <p> would be
  // auto-closed by a nested <div>).
  auto doc = html::Parser("<body><div>L<span style=\"display:inline-block;width:120px\">"
                          "<div>one</div><div>two</div></span>R</div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);
  dom::Element* el = dom::QuerySelector(*doc, "span");
  ASSERT_NE(el, nullptr);
  const InlineBox* holder = nullptr;
  const LayoutBox* block = FindInlineBlock(*root, el, holder);
  ASSERT_NE(block, nullptr);
  ASSERT_EQ(block->children.size(), 2u);
  EXPECT_GT(block->children[1]->y, block->children[0]->y); // stacks vertically
  EXPECT_LE(block->children[0]->width, 120.0f + 0.01f);
}

TEST(LayoutTest, InlineBlockNestedInsideInlineElement)
{
  auto doc =
      html::Parser("<body><p><span>a<span style=\"display:inline-block;width:40px;height:20px\">x"
                   "</span>b</span></p></body>")
          .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);
  const LayoutBox* p = FindBox(*root, "p", *doc);
  ASSERT_NE(p, nullptr);
  ASSERT_EQ(p->lines[0].boxes.size(), 1u);
  const InlineBox& ib = p->lines[0].boxes[0];
  ASSERT_NE(ib.block_box, nullptr);
  EXPECT_FLOAT_EQ(ib.width, 40.0f);
  EXPECT_FLOAT_EQ(ib.height, 20.0f);
}

TEST(LayoutTest, InlineBlockBorderBoxSitsAtMarginEdge)
{
  // Regression: the inner block's border-box origin is at the margin edge
  // (not shifted by -(border+padding), which pushed the left/top borders
  // off-canvas).
  auto doc = html::Parser("<body><div><span style=\"display:inline-block;border:3px solid #003366;"
                          "padding:10px\">x</span></div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 400);
  dom::Element* el = dom::QuerySelector(*doc, "span");
  ASSERT_NE(el, nullptr);
  const InlineBox* holder = nullptr;
  const LayoutBox* block = FindInlineBlock(*root, el, holder);
  ASSERT_NE(block, nullptr);
  const LayoutBox* div = FindBox(*root, "div", *doc);
  ASSERT_NE(div, nullptr);
  EXPECT_GE(block->x, div->content_x() - 0.01f); // left border on screen
  EXPECT_FLOAT_EQ(block->border_left, 3.0f);
  EXPECT_FLOAT_EQ(block->padding_left, 10.0f);
}

TEST(LayoutTest, InlineBlockExplicitHeightNotGrownByContent)
{
  // CSS2.2 10.6.2: a specified height wins; content taller overflows rather
  // than growing the box.
  auto doc = html::Parser("<body><div><span style=\"display:inline-block;width:60px;height:20px\">"
                          "tail<br>content</span></div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 400);
  dom::Element* el = dom::QuerySelector(*doc, "span");
  ASSERT_NE(el, nullptr);
  const InlineBox* holder = nullptr;
  const LayoutBox* block = FindInlineBlock(*root, el, holder);
  ASSERT_NE(block, nullptr);
  EXPECT_FLOAT_EQ(block->height, 20.0f);
}

TEST(LayoutTest, VerticalAlignEqualHeightBoxesFlatten)
{
  // Three inline-blocks of the same height (60px) with top/middle/bottom
  // vertical-align fill the same 60px line box: they all overlap (flatten) at
  // the same y instead of stacking their heights.
  auto doc = html::Parser("<body><div>"
                          "<span style=\"display:inline-block;width:40px;height:60px;"
                          "vertical-align:top\">t</span>"
                          "<span style=\"display:inline-block;width:40px;height:60px;"
                          "vertical-align:middle\">m</span>"
                          "<span style=\"display:inline-block;width:40px;height:60px;"
                          "vertical-align:bottom\">b</span>"
                          "</div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);
  const LayoutBox* div = FindBox(*root, "div", *doc);
  ASSERT_NE(div, nullptr);
  ASSERT_EQ(div->lines.size(), 1u);
  const Line& line = div->lines[0];
  ASSERT_EQ(line.boxes.size(), 3u);
  EXPECT_FLOAT_EQ(line.height, 60.0f);
  // All three boxes flatten to the same top y.
  EXPECT_FLOAT_EQ(line.boxes[0].y, line.boxes[1].y);
  EXPECT_FLOAT_EQ(line.boxes[1].y, line.boxes[2].y);
}

TEST(LayoutTest, WrappedInlineBlockRowsKeepLeadingGap)
{
  // Regression: baseline-aligned inline-blocks of height ~line-height must
  // still leave the strut's leading gap between wrapped rows, instead of the
  // rows touching (Y += box height with no leading).
  auto doc = html::Parser("<body><div style=\"width:200px\">"
                          "<span style=\"display:inline-block;width:110px;height:20px\">w110</span>"
                          "<span style=\"display:inline-block;width:110px;height:20px\">w110</span>"
                          "<span style=\"display:inline-block;width:110px;height:20px\">w110</span>"
                          "</div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);
  const LayoutBox* div = FindBox(*root, "div", *doc);
  ASSERT_NE(div, nullptr);
  // Three 110px boxes in a 200px container wrap onto three separate lines.
  ASSERT_EQ(div->lines.size(), 3u);
  const float gap01 = div->lines[1].boxes[0].y - div->lines[0].boxes[0].y;
  const float gap12 = div->lines[2].boxes[0].y - div->lines[1].boxes[0].y;
  // Each row is taller than its 20px box: the leading leaves a gap between
  // consecutive rows.
  EXPECT_GT(gap01, 20.0f);
  EXPECT_GT(gap12, 20.0f);
  EXPECT_LT(gap01, 20.0f + 4.0f); // a small (~1-2px) leading, not a whole box
  EXPECT_LT(gap12, 20.0f + 4.0f);
}

TEST(LayoutTest, FloatLeftHugsContainingBlockLeftEdge)
{
  // A float:left box is out of flow, placed at the block's left edge with its
  // specified size.
  auto doc = html::Parser("<body><div style=\"width:400px\">"
                          "<span style=\"float:left;width:100px;height:60px\">L</span>"
                          "text</div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);
  const LayoutBox* div = FindBox(*root, "div", *doc);
  ASSERT_NE(div, nullptr);
  ASSERT_EQ(div->floats.size(), 1u);
  const LayoutBox* f = div->floats[0].get();
  EXPECT_FLOAT_EQ(f->x, div->content_x()); // hugs the left edge
  EXPECT_FLOAT_EQ(f->width, 100.0f);
  EXPECT_FLOAT_EQ(f->height, 60.0f);
}

TEST(LayoutTest, FloatRightHugsContainingBlockRightEdge)
{
  auto doc = html::Parser("<body><div style=\"width:400px\">"
                          "<span style=\"float:right;width:80px;height:50px\">R</span>"
                          "text</div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);
  const LayoutBox* div = FindBox(*root, "div", *doc);
  ASSERT_NE(div, nullptr);
  ASSERT_EQ(div->floats.size(), 1u);
  const LayoutBox* f = div->floats[0].get();
  // Right edge of the float aligns with the containing block's content right.
  EXPECT_FLOAT_EQ(f->x + f->width, div->content_x() + div->content_width());
  EXPECT_FLOAT_EQ(f->width, 80.0f);
}

TEST(LayoutTest, FloatLeftWithPaddingStaysAtContentEdge)
{
  // A float:left with padding must sit its margin box at the containing
  // block's content left edge; its border box (and thus padding) extends
  // rightward from there and must not shift left out of the block.
  auto doc = html::Parser("<body><div style=\"width:400px\">"
                          "<span style=\"float:left;width:100px;height:50px;padding:10px\">L</span>"
                          "text</div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);
  const LayoutBox* div = FindBox(*root, "div", *doc);
  ASSERT_NE(div, nullptr);
  ASSERT_EQ(div->floats.size(), 1u);
  const LayoutBox* f = div->floats[0].get();
  // Border box origin at the content left edge (margin box == border box
  // when no margin): the box must not be shifted left by its padding.
  EXPECT_FLOAT_EQ(f->x, div->content_x());
  // Width includes the padding: 100 content + 10 + 10.
  EXPECT_FLOAT_EQ(f->width, 120.0f);
}

TEST(LayoutTest, FloatRightWithMarginKeepsRightEdge)
{
  // A float:right with a left margin must still align its right margin box
  // with the containing block's content right edge; the margin is taken out
  // of the space between, not by shifting the whole box left.
  auto doc =
      html::Parser("<body><div style=\"width:400px\">"
                   "<span style=\"float:right;width:80px;height:50px;margin-left:20px\">R</span>"
                   "text</div></body>")
          .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);
  const LayoutBox* div = FindBox(*root, "div", *doc);
  ASSERT_NE(div, nullptr);
  ASSERT_EQ(div->floats.size(), 1u);
  const LayoutBox* f = div->floats[0].get();
  // Right edge of the float (border box) aligns with the containing block's
  // content right edge.  The left margin sits between the float and the
  // content to its left; it does not shift the float off the right edge.
  EXPECT_FLOAT_EQ(f->x + f->width, div->content_x() + div->content_width());
}

TEST(LayoutTest, FloatInsideTranslatedInlineBlockKeepsPosition)
{
  // A float inside an inline-block is laid out at a local origin and then the
  // whole inline-block (including its floats) is translated to its final
  // position.  The float must move with its container.
  auto doc = html::Parser("<body><div style=\"width:400px;padding-top:100px\">"
                          "<span style=\"display:inline-block\">"
                          "<span style=\"float:left;width:50px;height:30px\">L</span>"
                          "</span></div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);

  // Find the inline-block span and the div.
  dom::Element* inline_span = dom::QuerySelector(*doc, "body div span");
  ASSERT_NE(inline_span, nullptr);
  const LayoutBox* div = FindBox(*root, "div", *doc);
  ASSERT_NE(div, nullptr);
  const InlineBox* holder = nullptr;
  const LayoutBox* ib = FindInlineBlock(*root, inline_span, holder);
  ASSERT_NE(ib, nullptr);
  ASSERT_EQ(ib->floats.size(), 1u);
  const LayoutBox* f = ib->floats[0].get();

  // The float's absolute position = inline-block's translated position + the
  // float's offset within it (the float hugs the inline-block's left edge).
  EXPECT_FLOAT_EQ(f->x, ib->x);
  // y must reflect the outer padding-top translation, not a local origin.
  EXPECT_FLOAT_EQ(f->y, ib->y);
}

TEST(LayoutTest, FloatShiftsFollowingLineStart)
{
  // A left float pushes the line box right so the text wraps around it.
  auto doc = html::Parser("<body><div style=\"width:400px\">"
                          "<span style=\"float:left;width:100px;height:60px\">L</span>"
                          "aaabbb cccddd eeefff</div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);
  const LayoutBox* div = FindBox(*root, "div", *doc);
  ASSERT_NE(div, nullptr);
  ASSERT_EQ(div->floats.size(), 1u);
  const LayoutBox* f = div->floats[0].get();
  ASSERT_FALSE(div->lines.empty());
  // The first line's first run starts to the right of the left float's edge.
  const float float_right = f->x + f->width;
  EXPECT_GE(div->lines[0].runs[0].x, float_right - 0.01f);
}

TEST(LayoutTest, CjkTextWrapsInsideFloatHeight)
{
  // CJK text may break anywhere, so a long run keeps wrapping on indented
  // lines (still inside the left float's height) rather than jumping back to
  // the left edge or overlapping the float.
  auto doc = html::Parser("<body><div style=\"width:300px\">"
                          "<span style=\"float:left;width:100px;height:80px\">L</span>"
                          "\xE5\x8F\xB3\xE4\xBE\xA7\xE6\x96\x87\xE5\xAD\x97"
                          "\xE5\x8F\xB3\xE4\xBE\xA7\xE6\x96\x87\xE5\xAD\x97"
                          "\xE5\x8F\xB3\xE4\xBE\xA7\xE6\x96\x87\xE5\xAD\x97"
                          "\xE5\x8F\xB3\xE4\xBE\xA7\xE6\x96\x87\xE5\xAD\x97"
                          "\xE5\x8F\xB3\xE4\xBE\xA7\xE6\x96\x87\xE5\xAD\x97"
                          "</div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);
  const LayoutBox* div = FindBox(*root, "div", *doc);
  ASSERT_NE(div, nullptr);
  ASSERT_EQ(div->floats.size(), 1u);
  const float float_bottom = div->floats[0]->y + div->floats[0]->height;
  const float float_right = div->floats[0]->x + div->floats[0]->width;
  ASSERT_GE(div->lines.size(), 2u);
  // Lines whose Y extent still overlaps the float stay indented to its right;
  // the first line does, and so does a later line still inside its height.
  for (const Line& l : div->lines) {
    for (const TextRun& r : l.runs) {
      if (r.y < float_bottom) {
        EXPECT_GE(r.x, float_right - 0.01f) << "run inside float height must indent";
      }
    }
  }
}

TEST(LayoutTest, FullwidthPunctuationBreaksCjkLines)
{
  // A paragraph that starts with a fullwidth comma (U+FF0C) — which used to be
  // treated as a non-CJK character — was measured as one unbreakable word and
  // overflowed its narrow container.  Fullwidth punctuation must break like
  // CJK so the text wraps inside the container.
  Page page = Build("<body><div style=\"width:120px\">"
                    "\xEF\xBC\x8C"                                     // ，fullwidth comma
                    "\xE4\xB8\xAD\xE6\x96\x87\xE6\x96\x87\xE5\xAD\x97" // 中文文字
                    "\xE4\xB8\xAD\xE6\x96\x87\xE6\x96\x87\xE5\xAD\x97" // 中文文字
                    "\xE4\xB8\xAD\xE6\x96\x87\xE6\x96\x87\xE5\xAD\x97" // 中文文字
                    "\xE4\xB8\xAD\xE6\x96\x87\xE6\x96\x87\xE5\xAD\x97" // 中文文字
                    "\xE4\xB8\xAD\xE6\x96\x87\xE6\x96\x87\xE5\xAD\x97" // 中文文字
                    "</div></body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  ASSERT_GT(div->lines.size(), 1u);
  const float right_edge = div->x + div->width;
  for (const Line& line : div->lines) {
    for (const TextRun& run : line.runs) {
      EXPECT_LE(run.x + run.width, right_edge + 0.01f) << "CJK run must not overflow its container";
    }
  }
}

TEST(LayoutTest, BlockChildTextAvoidsParentFloat)
{
  // A float belongs to the enclosing block formatting context: the text of a
  // block-level child (e.g. <h3>) that vertically overlaps the float must
  // indent past it, not render underneath it.
  auto doc = html::Parser("<body><div style=\"width:500px\">"
                          "<span style=\"float:left;width:120px;height:80px\">L</span>"
                          "<h3>\xE6\xA0\x87\xE9\xA2\x98\xE6\x96\x87\xE6\x9C\xAC</h3>"
                          "</div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine engine(styles);
  auto root = engine.BuildLayoutTree(*doc, 800);
  const LayoutBox* div = FindBox(*root, "div", *doc);
  ASSERT_NE(div, nullptr);
  ASSERT_EQ(div->floats.size(), 1u);
  const float float_right = div->floats[0]->x + div->floats[0]->width;
  const float float_bottom = div->floats[0]->y + div->floats[0]->height;
  const LayoutBox* h3 = FindBox(*root, "h3", *doc);
  ASSERT_NE(h3, nullptr);
  ASSERT_FALSE(h3->lines.empty());
  // The h3 text overlaps the float vertically and must start to its right.
  for (const Line& l : h3->lines) {
    for (const TextRun& r : l.runs) {
      if (r.y < float_bottom) {
        EXPECT_GE(r.x, float_right - 0.01f);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Flex layout (monospace fallback: each character is font_size px wide)
// ---------------------------------------------------------------------------

TEST(FlexLayoutTest, ItemsFlowInRow)
{
  Page page = Build("<body><div style=\"display:flex\"><div>a</div><div>b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  const LayoutBox* a = flex->children[0].get();
  const LayoutBox* b = flex->children[1].get();
  EXPECT_FLOAT_EQ(a->width, 16.0f); // "a" at 16px
  EXPECT_FLOAT_EQ(b->width, 16.0f);
  EXPECT_FLOAT_EQ(a->x, 8.0f); // container content starts at body content x
  EXPECT_FLOAT_EQ(b->x, 24.0f);
  EXPECT_FLOAT_EQ(a->y, b->y);
}

TEST(FlexLayoutTest, FlexGrowDistributesFreeSpace)
{
  Page page = Build("<body><div style=\"display:flex\">"
                    "<div style=\"flex-grow:1\">a</div>"
                    "<div style=\"flex-grow:2\">b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  const LayoutBox* a = flex->children[0].get();
  const LayoutBox* b = flex->children[1].get();
  // Container content width 784; items 16 each; free 752 split 1:2.
  EXPECT_NEAR(a->width, 16.0f + 752.0f / 3.0f, 0.01f);
  EXPECT_NEAR(b->width, 16.0f + 752.0f * 2.0f / 3.0f, 0.01f);
  EXPECT_NEAR(b->x, 8.0f + a->width, 0.01f);
}

TEST(FlexLayoutTest, FlexShrinkReducesOverflow)
{
  Page page = Build("<body><div style=\"display:flex; width:100px\">"
                    "<div style=\"width:80px\">a</div>"
                    "<div style=\"width:80px\">b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  const LayoutBox* a = flex->children[0].get();
  const LayoutBox* b = flex->children[1].get();
  // Container 100, outer sum 160, free -60, equal shrink weights -> 50 each.
  EXPECT_NEAR(a->width, 50.0f, 0.01f);
  EXPECT_NEAR(b->width, 50.0f, 0.01f);
}

TEST(FlexLayoutTest, FlexBasisSizesItem)
{
  Page page = Build("<body><div style=\"display:flex\">"
                    "<div style=\"flex-basis:100px\">a</div>"
                    "<div style=\"flex-basis:50px\">b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  EXPECT_FLOAT_EQ(flex->children[0]->width, 100.0f);
  EXPECT_FLOAT_EQ(flex->children[1]->width, 50.0f);
  EXPECT_FLOAT_EQ(flex->children[1]->x, 108.0f);
}

TEST(FlexLayoutTest, FlexShorthand)
{
  // flex:1 == grow 1, shrink 1, basis 0: items share the width equally.
  Page page = Build("<body><div style=\"display:flex\">"
                    "<div style=\"flex:1\">a</div>"
                    "<div style=\"flex:1\">b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  EXPECT_NEAR(flex->children[0]->width, 392.0f, 0.01f);
  EXPECT_NEAR(flex->children[1]->width, 392.0f, 0.01f);
}

TEST(FlexLayoutTest, JustifyContentCenter)
{
  Page page =
      Build("<body><div style=\"display:flex; justify-content:center\"><div>a</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 1u);
  EXPECT_FLOAT_EQ(flex->children[0]->x, 8.0f + (784.0f - 16.0f) / 2.0f);
}

TEST(FlexLayoutTest, JustifyContentFlexEnd)
{
  Page page = Build(
      "<body><div style=\"display:flex; justify-content:flex-end\"><div>a</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 1u);
  EXPECT_FLOAT_EQ(flex->children[0]->x, 8.0f + 784.0f - 16.0f);
}

TEST(FlexLayoutTest, JustifyContentSpaceBetween)
{
  Page page = Build("<body><div style=\"display:flex; justify-content:space-between\">"
                    "<div>a</div><div>b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  EXPECT_FLOAT_EQ(flex->children[0]->x, 8.0f);
  EXPECT_FLOAT_EQ(flex->children[1]->x, 8.0f + 784.0f - 16.0f);
}

TEST(FlexLayoutTest, ColumnStacksVertically)
{
  Page page = Build("<body><div style=\"display:flex; flex-direction:column\">"
                    "<div>a</div><div>b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  const LayoutBox* a = flex->children[0].get();
  const LayoutBox* b = flex->children[1].get();
  EXPECT_FLOAT_EQ(a->x, b->x);
  EXPECT_FLOAT_EQ(a->y, 8.0f);
  EXPECT_FLOAT_EQ(b->y, 8.0f + a->height);
}

TEST(FlexLayoutTest, AlignItemsCenter)
{
  Page page = Build("<body><div style=\"display:flex; align-items:center; height:100px\">"
                    "<div style=\"height:20px\">a</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 1u);
  // Single line stretches to the container height; the item is centered.
  EXPECT_FLOAT_EQ(flex->children[0]->y, 8.0f + (100.0f - 20.0f) / 2.0f);
}

TEST(FlexLayoutTest, AlignItemsStretchDefault)
{
  Page page = Build("<body><div style=\"display:flex\">"
                    "<div style=\"height:50px\">a</div><div>b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  EXPECT_FLOAT_EQ(flex->children[0]->height, 50.0f);
  // The auto-height item stretches to the line's cross size.
  EXPECT_FLOAT_EQ(flex->children[1]->height, 50.0f);
}

TEST(FlexLayoutTest, FlexWrapCreatesLines)
{
  Page page = Build("<body><div style=\"display:flex; flex-wrap:wrap; width:100px\">"
                    "<div style=\"width:60px\">a</div>"
                    "<div style=\"width:30px\">b</div>"
                    "<div style=\"width:60px\">c</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 3u);
  const LayoutBox* a = flex->children[0].get();
  const LayoutBox* b = flex->children[1].get();
  const LayoutBox* c = flex->children[2].get();
  // Line 1 = [a, b] (60 + 30 = 90 <= 100); line 2 = [c].
  EXPECT_FLOAT_EQ(a->x, 8.0f);
  EXPECT_FLOAT_EQ(b->x, 8.0f + 60.0f);
  EXPECT_FLOAT_EQ(c->x, 8.0f);
  EXPECT_FLOAT_EQ(a->y, 8.0f);
  EXPECT_FLOAT_EQ(c->y, 8.0f + a->height);
}

TEST(FlexLayoutTest, GapSpacesItems)
{
  Page page = Build("<body><div style=\"display:flex; gap:10px\">"
                    "<div>a</div><div>b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  EXPECT_FLOAT_EQ(flex->children[0]->x, 8.0f);
  EXPECT_FLOAT_EQ(flex->children[1]->x, 8.0f + 16.0f + 10.0f);
}

TEST(FlexLayoutTest, RowReverseMirrorsItems)
{
  Page page = Build("<body><div style=\"display:flex; flex-direction:row-reverse\">"
                    "<div>a</div><div>b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  // "a" is the first item but appears at the right (main-end) edge.
  EXPECT_FLOAT_EQ(flex->children[0]->x, 8.0f + 784.0f - 16.0f);
  EXPECT_FLOAT_EQ(flex->children[1]->x, 8.0f + 784.0f - 32.0f);
}

TEST(FlexLayoutTest, OverflowWithJustifyFlexEndPacksAtStart)
{
  // Regression: with negative free space (items do not shrink), every
  // justify-content value must pack toward main-start (CSS Flexbox 1 §8.2).
  // Previously flex-end set a negative cursor, pushing items off the left
  // edge of the container.
  Page page = Build("<body><div style=\"display:flex; width:100px; justify-content:flex-end\">"
                    "<div style=\"width:80px; flex-shrink:0\">a</div>"
                    "<div style=\"width:80px; flex-shrink:0\">b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  const LayoutBox* a = flex->children[0].get();
  const LayoutBox* b = flex->children[1].get();
  // Packed from the start; the overflow runs off the right edge.
  EXPECT_FLOAT_EQ(a->x, 8.0f);
  EXPECT_FLOAT_EQ(b->x, 8.0f + 80.0f);
}

TEST(FlexLayoutTest, ShrinkWeightUsesContentBoxBase)
{
  // Regression: the scaled flex shrink factor uses the content-box flex base
  // size (CSS Flexbox 1 §9.7), not border+padding+content.  Two items with
  // equal content (60px) but different borders must shrink to equal content
  // widths even though their outer sizes differ.
  Page page = Build("<body><div style=\"display:flex; width:100px\">"
                    "<div style=\"width:60px; border:10px solid\">a</div>"
                    "<div style=\"width:60px\">b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  const LayoutBox* a = flex->children[0].get();
  const LayoutBox* b = flex->children[1].get();
  // Outer sizes: a = 60+20 = 80, b = 60.  Total 140, container 100, free -40.
  // Equal content-box weights (60/60) split the reduction 20/20.
  EXPECT_NEAR(a->content_width(), 40.0f, 0.01f);
  EXPECT_NEAR(b->content_width(), 40.0f, 0.01f);
  // Both outer boxes fit the 100px container exactly.
  EXPECT_NEAR(a->width + b->width, 100.0f, 0.01f);
}

TEST(FlexLayoutTest, OrderReordersItems)
{
  Page page = Build("<body><div style=\"display:flex\">"
                    "<div id=\"a\" style=\"order:2\">a</div>"
                    "<div id=\"b\" style=\"order:1\">b</div>"
                    "<div id=\"c\" style=\"order:1\">c</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 3u);
  // Ascending order; equal order keeps document order (b before c).
  EXPECT_EQ(flex->children[0]->element->Id().value(), "b");
  EXPECT_EQ(flex->children[1]->element->Id().value(), "c");
  EXPECT_EQ(flex->children[2]->element->Id().value(), "a");
  EXPECT_FLOAT_EQ(flex->children[0]->x, 8.0f);
  EXPECT_FLOAT_EQ(flex->children[1]->x, 24.0f);
  EXPECT_FLOAT_EQ(flex->children[2]->x, 40.0f);
}

TEST(FlexLayoutTest, AlignSelfOverridesAlignItems)
{
  Page page = Build("<body><div style=\"display:flex; align-items:center; height:100px\">"
                    "<div style=\"height:20px\">a</div>"
                    "<div style=\"height:20px; align-self:flex-start\">b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  // Default align-items centers both, but align-self:flex-start pins b to the
  // cross-start (top) edge.
  EXPECT_FLOAT_EQ(flex->children[0]->y, 8.0f + (100.0f - 20.0f) / 2.0f);
  EXPECT_FLOAT_EQ(flex->children[1]->y, 8.0f);
}

TEST(FlexLayoutTest, AlignSelfStretchOverridePreventsStretch)
{
  // align-self:flex-start also disables the default stretch for an
  // auto-height item.
  Page page = Build("<body><div style=\"display:flex\">"
                    "<div style=\"height:50px\">a</div>"
                    "<div style=\"align-self:flex-start\">b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  EXPECT_FLOAT_EQ(flex->children[0]->height, 50.0f);
  // b is not stretched: its height stays at the line-height content box.
  EXPECT_LT(flex->children[1]->height, 50.0f);
}

TEST(FlexLayoutTest, MinWidthClampsShrink)
{
  Page page = Build("<body><div style=\"display:flex; width:100px\">"
                    "<div style=\"width:80px; min-width:60px\">a</div>"
                    "<div style=\"width:80px\">b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  // Equal shrink (both base 80) would give 50/50, but a's min-width floors it
  // at 60; b takes the remaining reduction.
  EXPECT_NEAR(flex->children[0]->width, 60.0f, 0.01f);
  EXPECT_NEAR(flex->children[1]->width, 50.0f, 0.01f);
}

TEST(FlexLayoutTest, MaxWidthClampsGrow)
{
  Page page = Build("<body><div style=\"display:flex\">"
                    "<div style=\"width:16px; max-width:20px; flex-grow:1\">a</div>"
                    "<div style=\"width:16px; flex-grow:1\">b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  // Equal growth would give both ~392, but a is capped at its 20px max-width
  // (the freed space is not re-distributed — documented limitation).
  EXPECT_NEAR(flex->children[0]->width, 20.0f, 0.01f);
  EXPECT_NEAR(flex->children[1]->width, 16.0f + 752.0f / 2.0f, 0.01f);
}

TEST(FlexLayoutTest, AutoMarginCentersOnMainAxis)
{
  // margin: auto on the main axis absorbs the free space, centering the item
  // (CSS Flexbox 1 §8.1); it overrides justify-content.
  Page page = Build("<body><div style=\"display:flex\">"
                    "<div style=\"margin:0 auto\">a</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 1u);
  // (784 - 16) / 2 = 384 on each side.
  EXPECT_NEAR(flex->children[0]->x, 8.0f + 384.0f, 0.01f);
}

TEST(FlexLayoutTest, AutoMarginOnCrossAxis)
{
  Page page = Build("<body><div style=\"display:flex; height:100px\">"
                    "<div style=\"margin-top:auto; margin-bottom:auto; height:20px\">a</div>"
                    "</div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 1u);
  // (100 - 20) / 2 = 40 pushed down by the auto top margin.
  EXPECT_NEAR(flex->children[0]->y, 8.0f + 40.0f, 0.01f);
}

TEST(FlexLayoutTest, ColumnMinHeightClamps)
{
  Page page = Build("<body><div style=\"display:flex; flex-direction:column; height:100px\">"
                    "<div style=\"height:80px; min-height:60px\">a</div>"
                    "<div style=\"height:80px\">b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  // Column main axis is height: equal shrink would give 50/50, but a's
  // min-height floors it at 60.
  EXPECT_NEAR(flex->children[0]->height, 60.0f, 0.01f);
  EXPECT_NEAR(flex->children[1]->height, 50.0f, 0.01f);
}

TEST(FlexLayoutTest, AlignSelfOnColumnCrossAxis)
{
  // In a column, align-self acts on the horizontal (cross) axis.
  Page page = Build("<body><div style=\"display:flex; flex-direction:column; "
                    "align-items:flex-start; width:200px\">"
                    "<div style=\"width:60px\">a</div>"
                    "<div style=\"width:60px; align-self:flex-end\">b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  // a at the cross-start (left); b pinned to the cross-end (right edge).
  EXPECT_FLOAT_EQ(flex->children[0]->x, 8.0f);
  EXPECT_FLOAT_EQ(flex->children[1]->x, 8.0f + 200.0f - 60.0f);
}

TEST(FlexLayoutTest, PercentHeightContainerDoesNotCollapse)
{
  // A column flex container with height:50% is indefinite (the engine does
  // not resolve the parent height), so it must size to content rather than
  // collapsing to a definite 0.
  Page page = Build("<body><div style=\"display:flex;flex-direction:column;height:50%\">"
                    "<div style=\"height:30px\">a</div>"
                    "<div style=\"height:40px\">b</div></div></body>");
  const LayoutBox* flex = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(flex, nullptr);
  ASSERT_EQ(flex->children.size(), 2u);
  // Container height follows content (30 + 40 = 70), not 0.
  EXPECT_GT(flex->height, 0.0f);
  EXPECT_FLOAT_EQ(flex->height, 70.0f);
}

// Grid layout (monospace fallback: each character is font_size px wide)
// ---------------------------------------------------------------------------

TEST(GridLayoutTest, FrColumnsShareContainerWidth)
{
  Page page = Build("<body><div style=\"display:grid; grid-template-columns:1fr 1fr\">"
                    "<div>a</div><div>b</div></div></body>");
  const LayoutBox* grid = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(grid, nullptr);
  ASSERT_EQ(grid->children.size(), 2u);
  // Container content width 784 split equally between the two 1fr columns.
  EXPECT_NEAR(grid->children[0]->x, 8.0f, 0.01f);
  EXPECT_NEAR(grid->children[0]->width, 392.0f, 0.01f);
  EXPECT_NEAR(grid->children[1]->x, 8.0f + 392.0f, 0.01f);
  EXPECT_NEAR(grid->children[1]->width, 392.0f, 0.01f);
}

TEST(GridLayoutTest, FixedColumnsKeepTheirSize)
{
  Page page = Build("<body><div style=\"display:grid; grid-template-columns:100px 200px\">"
                    "<div>a</div><div>b</div></div></body>");
  const LayoutBox* grid = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(grid, nullptr);
  ASSERT_EQ(grid->children.size(), 2u);
  EXPECT_FLOAT_EQ(grid->children[0]->x, 8.0f);
  EXPECT_FLOAT_EQ(grid->children[0]->width, 100.0f);
  EXPECT_FLOAT_EQ(grid->children[1]->x, 8.0f + 100.0f);
  EXPECT_FLOAT_EQ(grid->children[1]->width, 200.0f);
}

TEST(GridLayoutTest, AutoPlacementWrapsRows)
{
  // Three items in a two-column grid: two on the first row, one on the second.
  Page page = Build("<body><div style=\"display:grid; grid-template-columns:100px 100px\">"
                    "<div>a</div><div>b</div><div>c</div></div></body>");
  const LayoutBox* grid = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(grid, nullptr);
  ASSERT_EQ(grid->children.size(), 3u);
  EXPECT_FLOAT_EQ(grid->children[0]->x, 8.0f);
  EXPECT_FLOAT_EQ(grid->children[0]->y, 8.0f);
  EXPECT_FLOAT_EQ(grid->children[1]->x, 8.0f + 100.0f);
  EXPECT_FLOAT_EQ(grid->children[1]->y, 8.0f);
  // c wraps to the second row; the first row is ~line-height tall.
  EXPECT_FLOAT_EQ(grid->children[2]->x, 8.0f);
  EXPECT_NEAR(grid->children[2]->y, 8.0f + 19.2f, 0.01f);
}

TEST(GridLayoutTest, SpanAcrossColumns)
{
  Page page = Build("<body><div style=\"display:grid; grid-template-columns:100px 100px 100px\">"
                    "<div style=\"grid-column: span 2\">a</div>"
                    "<div>b</div></div></body>");
  const LayoutBox* grid = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(grid, nullptr);
  ASSERT_EQ(grid->children.size(), 2u);
  // a spans columns 0-1 (200px); b flows into column 2.
  EXPECT_FLOAT_EQ(grid->children[0]->x, 8.0f);
  EXPECT_FLOAT_EQ(grid->children[0]->width, 200.0f);
  EXPECT_FLOAT_EQ(grid->children[1]->x, 8.0f + 200.0f);
  EXPECT_FLOAT_EQ(grid->children[1]->width, 100.0f);
}

TEST(GridLayoutTest, ColumnAndRowGap)
{
  Page page = Build("<body><div style=\"display:grid; grid-template-columns:100px 100px; "
                    "grid-template-rows:40px 40px; gap:10px\">"
                    "<div>a</div><div>b</div><div>c</div><div>d</div></div></body>");
  const LayoutBox* grid = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(grid, nullptr);
  ASSERT_EQ(grid->children.size(), 4u);
  // b is shifted by column 1 + gap; c starts the second row + gap.
  EXPECT_FLOAT_EQ(grid->children[1]->x, 8.0f + 100.0f + 10.0f);
  EXPECT_FLOAT_EQ(grid->children[2]->y, 8.0f + 40.0f + 10.0f);
}

TEST(GridLayoutTest, ExplicitLinePlacement)
{
  Page page = Build("<body><div style=\"display:grid; grid-template-columns:100px 100px 100px\">"
                    "<div style=\"grid-column: 2\">a</div>"
                    "<div style=\"grid-column: 3\">b</div></div></body>");
  const LayoutBox* grid = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(grid, nullptr);
  ASSERT_EQ(grid->children.size(), 2u);
  // 1-based lines: column 2 => 0-based index 1.
  EXPECT_FLOAT_EQ(grid->children[0]->x, 8.0f + 100.0f);
  EXPECT_FLOAT_EQ(grid->children[1]->x, 8.0f + 200.0f);
}

TEST(GridLayoutTest, AutoRowsSizeToContent)
{
  Page page = Build("<body><div style=\"display:grid; grid-template-columns:100px\">"
                    "<div>a</div>"
                    "<div style=\"height:50px\">b</div></div></body>");
  const LayoutBox* grid = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(grid, nullptr);
  ASSERT_EQ(grid->children.size(), 2u);
  // Row 0 (auto) sizes to item a's ~line-height; b starts below it.
  EXPECT_NEAR(grid->children[1]->y, 8.0f + 19.2f, 0.01f);
  EXPECT_FLOAT_EQ(grid->children[1]->height, 50.0f);
}

TEST(GridLayoutTest, MixedFrAndFixedColumns)
{
  Page page = Build("<body><div style=\"display:grid; grid-template-columns:100px 1fr\">"
                    "<div>a</div><div>b</div></div></body>");
  const LayoutBox* grid = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(grid, nullptr);
  ASSERT_EQ(grid->children.size(), 2u);
  // The fr column takes the leftover: 784 - 100.
  EXPECT_FLOAT_EQ(grid->children[0]->width, 100.0f);
  EXPECT_NEAR(grid->children[1]->width, 684.0f, 0.01f);
  EXPECT_FLOAT_EQ(grid->children[1]->x, 8.0f + 100.0f);
}

} // namespace
} // namespace neko::layout
