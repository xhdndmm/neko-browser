#include "neko/layout/layout_tree.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "neko/dom/element.h"

namespace neko::layout {
namespace {

// A unit of inline content (a text chunk with its style).
struct InlineItem {
  std::string text;
  const style::ComputedStyle* style;
  const dom::Element* element;  // source element (null for block-level text)
};

// True when |c| is an ASCII whitespace character used for word breaking.
bool IsWordBreak(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

float ResolveSize(const style::SizeSpec& spec, float containing) {
  if (spec.percent) {
    return containing * spec.value / 100.0f;
  }
  return spec.value;
}

void CollectText(std::string_view text, const style::ComputedStyle& style,
                 const dom::Element* element, std::vector<InlineItem>& items) {
  if (!text.empty()) {
    items.push_back(InlineItem{std::string(text), &style, element});
  }
}

// Collects inline content: text nodes and inline elements (recursively).
void CollectInline(dom::Node& node, const style::ComputedStyle& style, const dom::Element* element,
                   const style::StyleEngine& styles, std::vector<InlineItem>& items) {
  if (node.node_type() == dom::NodeType::kText) {
    CollectText(static_cast<const dom::Text&>(node).data(), style, element, items);
    return;
  }
  if (node.node_type() != dom::NodeType::kElement) {
    return;
  }
  const dom::Element& child_element = static_cast<const dom::Element&>(node);
  const style::ComputedStyle& child_style = styles.StyleFor(child_element);
  for (dom::Node* child : node.ChildNodes()) {
    CollectInline(*child, child_style, &child_element, styles, items);
  }
}

// Breaks inline items into wrapped lines and fills |out_lines|.  Positions are
// relative to the box (origin_x/origin_y are the content box origin).
void LayoutLines(const std::vector<InlineItem>& items, float available_width, float origin_x,
                 float origin_y, std::vector<Line>& out_lines, float& total_height) {
  Line line;
  float x = 0;
  float line_top = 0;

  auto flush_line = [&]() {
    if (line.runs.empty() && out_lines.empty()) {
      return;
    }
    if (line.runs.empty()) {
      // Trailing whitespace-only line: ignore.
      return;
    }
    // Position runs vertically within the line.
    for (TextRun& run : line.runs) {
      run.y = origin_y + line_top + (line.height - run.font_size) / 2.0f;
      run.x = origin_x + run.x;
    }
    out_lines.push_back(std::move(line));
    line = Line{};
    total_height += out_lines.back().height;
    line_top += out_lines.back().height;
    x = 0;
  };

  auto add_word = [&](std::string_view word, const style::ComputedStyle& style,
                      const dom::Element* element) {
    const float word_width = static_cast<float>(word.size()) * style.font_size;
    if (x + word_width > available_width && x > 0) {
      flush_line();
    }
    TextRun run;
    run.text = std::string(word);
    run.x = x;
    run.font_size = style.font_size;
    run.color = style.color.value_or(css::Color{0, 0, 0, 255});
    run.underline = style.text_decoration_underline;
    run.element = element;
    line.runs.push_back(std::move(run));
    line.height = std::max(line.height, style.line_height);
    x += word_width;
  };

  for (const InlineItem& item : items) {
    const style::ComputedStyle& style = *item.style;
    std::size_t start = 0;
    const std::string& text = item.text;
    while (start < text.size()) {
      while (start < text.size() && IsWordBreak(text[start])) {
        const float space_width = style.font_size * 0.5f;
        if (x + space_width > available_width && x > 0) {
          flush_line();
        }
        x += space_width;
        ++start;
      }
      if (start >= text.size()) {
        break;
      }
      const std::size_t end = text.find_first_of(" \t\n\r", start);
      const std::string_view word = std::string_view(text).substr(
          start, end == std::string::npos ? text.size() - start : end - start);
      add_word(word, style, item.element);
      start = end == std::string::npos ? text.size() : end;
    }
  }
  flush_line();
}

}  // namespace

std::unique_ptr<LayoutBox> LayoutEngine::BuildLayoutTree(dom::Document& document,
                                                         float viewport_width) {
  // Coordinates are absolute (viewport space).  BuildBlock lays out |element|
  // inside a containing block whose content box starts at |origin_x|/|origin_y|.
  struct Builder {
    const style::StyleEngine& styles;

    std::unique_ptr<LayoutBox> BuildBlock(dom::Element& element, float containing_width,
                                          float origin_x, float origin_y) {
      auto box = std::make_unique<LayoutBox>();
      box->element = &element;
      box->style = styles.StyleFor(element);

      box->margin_top = ResolveSize(box->style.margin_top, containing_width);
      box->margin_right = ResolveSize(box->style.margin_right, containing_width);
      box->margin_bottom = ResolveSize(box->style.margin_bottom, containing_width);
      box->margin_left = ResolveSize(box->style.margin_left, containing_width);
      box->border_top = ResolveSize(box->style.border_top, containing_width);
      box->border_right = ResolveSize(box->style.border_right, containing_width);
      box->border_bottom = ResolveSize(box->style.border_bottom, containing_width);
      box->border_left = ResolveSize(box->style.border_left, containing_width);
      box->padding_top = ResolveSize(box->style.padding_top, containing_width);
      box->padding_right = ResolveSize(box->style.padding_right, containing_width);
      box->padding_bottom = ResolveSize(box->style.padding_bottom, containing_width);
      box->padding_left = ResolveSize(box->style.padding_left, containing_width);

      // Width: explicit (px or %) or fill the containing block.
      float content_width = 0;
      if (box->style.width.has_value()) {
        content_width = ResolveSize(box->style.width.value(), containing_width);
      } else {
        content_width = containing_width - box->margin_left - box->margin_right -
                        box->border_left - box->border_right - box->padding_left -
                        box->padding_right;
        if (content_width < 0) {
          content_width = 0;
        }
      }
      box->width = content_width + box->border_left + box->border_right + box->padding_left +
                   box->padding_right;

      // Border-box position, including the relative offset.  |origin_x| is the
      // content-box origin of the parent; this box's own margin pushes it out.
      float rel_x = 0;
      float rel_y = 0;
      if (box->style.position == style::Position::kRelative) {
        rel_x = box->style.left;
        rel_y = box->style.top;
      }
      box->x = origin_x + box->margin_left - box->border_left - box->padding_left + rel_x;
      box->y = origin_y + box->margin_top - box->border_top - box->padding_top + rel_y;

      const float content_x = origin_x + box->margin_left + rel_x;
      const float content_y = origin_y + box->margin_top + rel_y;
      const float avail_width = box->width - box->border_left - box->border_right -
                                box->padding_left - box->padding_right;

      // Lay out children.
      float cursor_y = 0;
      std::vector<InlineItem> inline_items;
      for (dom::Node* child : element.ChildNodes()) {
        if (child->node_type() == dom::NodeType::kText) {
          CollectText(static_cast<dom::Text*>(child)->data(), box->style, &element, inline_items);
          continue;
        }
        if (child->node_type() != dom::NodeType::kElement) {
          continue;
        }
        dom::Element& child_element = *static_cast<dom::Element*>(child);
        const style::ComputedStyle& child_style = styles.StyleFor(child_element);
        if (child_style.display == style::Display::kNone) {
          continue;
        }
        if (child_style.display == style::Display::kBlock) {
          std::unique_ptr<LayoutBox> child_box =
              BuildBlock(child_element, avail_width, content_x, content_y + cursor_y);
          cursor_y += child_box->margin_top + child_box->height + child_box->margin_bottom;
          box->children.push_back(std::move(child_box));
        } else {
          // Inline element: its text flows into this box's lines.
          CollectInline(child_element, child_style, &child_element, styles, inline_items);
        }
      }

      // Inline layout.
      float lines_height = 0;
      LayoutLines(inline_items, avail_width, content_x, content_y + cursor_y, box->lines,
                  lines_height);
      cursor_y += lines_height;

      // Height: content-based, or an explicit value.
      float content_height = cursor_y;
      if (box->style.height.has_value() && !box->style.height.value().percent) {
        content_height = std::max(content_height, box->style.height.value().value);
      }
      box->height = content_height + box->border_top + box->border_bottom + box->padding_top +
                    box->padding_bottom;
      return box;
    }
  };

  dom::Element* root = document.document_element();
  if (root == nullptr) {
    return nullptr;
  }
  Builder builder{styles_};
  return builder.BuildBlock(*root, viewport_width, 0, 0);
}

}  // namespace neko::layout
