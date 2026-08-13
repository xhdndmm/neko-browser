#include "neko/layout/layout_tree.h"

#include <memory>
#include <string>
#include <string_view>

#include "neko/dom/query.h"
#include "neko/html/parser.h"
#include "neko/style/style_engine.h"
#include <gtest/gtest.h>

namespace neko::layout {
namespace {

struct Page {
  std::unique_ptr<dom::Document> doc;
  style::StyleEngine styles;
  std::unique_ptr<LayoutBox> root;
};

Page Build(std::string_view html, float viewport = 800.0f) {
  Page page;
  page.doc = html::Parser(html).Parse();
  page.styles.ApplyStyles(*page.doc);
  layout::LayoutEngine engine(page.styles);
  page.root = engine.BuildLayoutTree(*page.doc, viewport);
  return page;
}

const LayoutBox* FindBox(const LayoutBox& box, std::string_view selector,
                         dom::Document& doc) {
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

TEST(LayoutTest, BlockFillsContainingBlock) {
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

TEST(LayoutTest, VerticalStacking) {
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

TEST(LayoutTest, BoxModelWithPaddingBorderMargin) {
  Page page = Build(
      "<body><div style=\"width: 200px; padding: 10px; border: 2px solid; "
      "margin: 5px\">x</div></body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  EXPECT_FLOAT_EQ(div->width, 224.0f);   // 200 + 2*10 + 2*2
  EXPECT_FLOAT_EQ(div->content_width(), 200.0f);
  EXPECT_FLOAT_EQ(div->margin_top, 5.0f);
  EXPECT_FLOAT_EQ(div->border_top, 2.0f);
  EXPECT_FLOAT_EQ(div->padding_top, 10.0f);
}

TEST(LayoutTest, ExplicitHeight) {
  Page page = Build("<body><div style=\"height: 100px\">x</div></body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  EXPECT_FLOAT_EQ(div->height, 100.0f);
}

TEST(LayoutTest, PercentWidth) {
  Page page = Build("<body><div style=\"width: 50%\">x</div></body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  // 50% of body content width (784).
  EXPECT_FLOAT_EQ(div->width, 392.0f);
}

TEST(LayoutTest, InlineTextCreatesLines) {
  Page page = Build("<body><p>hello world</p></body>");
  const LayoutBox* p = FindBox(*page.root, "p", *page.doc);
  ASSERT_NE(p, nullptr);
  ASSERT_FALSE(p->lines.empty());
  ASSERT_GE(p->lines[0].runs.size(), 1u);
  EXPECT_EQ(p->lines[0].runs[0].text, "hello");
  // Default font size 16 -> line height 19.2.
  EXPECT_FLOAT_EQ(p->lines[0].height, 19.2f);
}

TEST(LayoutTest, TextWrapsAtWordBoundary) {
  // 100px content width fits one 16px-wide word per line at most.
  Page page = Build(
      "<body><div style=\"width: 100px\">one two three four</div></body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  ASSERT_GE(div->lines.size(), 2u);  // wrapped into multiple lines
  EXPECT_LE(div->lines[0].runs.size(), 2u);
}

TEST(LayoutTest, DisplayNoneSkipped) {
  Page page = Build("<body><div>visible</div><div style=\"display:none\">hidden</div></body>");
  const LayoutBox* body = FindBox(*page.root, "body", *page.doc);
  ASSERT_NE(body, nullptr);
  ASSERT_EQ(body->children.size(), 1u);
  EXPECT_EQ(body->children[0]->element->tag_name(), "div");
}

TEST(LayoutTest, InlineElementStyleAppliesToText) {
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

TEST(LayoutTest, RelativePositionShift) {
  Page page = Build("<body><div style=\"position: relative; top: 10px; left: 5px\">x</div></body>");
  const LayoutBox* div = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(div, nullptr);
  EXPECT_FLOAT_EQ(div->y, 8.0f + 10.0f);  // body content y (8) + offset
}

TEST(LayoutTest, NestedBlockHeightInherits) {
  Page page = Build("<body><div><div>text</div></div></body>");
  const LayoutBox* outer = FindBox(*page.root, "div", *page.doc);
  ASSERT_NE(outer, nullptr);
  ASSERT_EQ(outer->children.size(), 1u);
  const LayoutBox* inner = outer->children[0].get();
  // outer height = inner height (both content-based).
  EXPECT_FLOAT_EQ(outer->height, inner->height);
}

}  // namespace
}  // namespace neko::layout
