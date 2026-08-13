#include "neko/layout/layout_tree.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "neko/dom/element.h"
#include "neko/graphics/font_registry.h"
#include "neko/graphics/font_selector.h"

namespace neko::layout {
namespace {

// A unit of inline content (a text chunk with its style).
struct InlineItem {
  std::string text;
  const style::ComputedStyle* style;
  const dom::Element* element;  // source element (null for block-level text)
  bool line_break = false;      // <br>: force a line break
};

// True when |c| is an ASCII whitespace character used for word breaking.
bool IsWordBreak(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Measures the advance width of |text| at |font_size| using the font selector
// for |family| when a registry is available; falls back to the monospace model
// (font_size per character).
float MeasureTextWidth(const graphics::FontRegistry* registry, std::string_view family,
                       std::string_view text, float font_size) {
  if (registry == nullptr) {
    return static_cast<float>(text.size()) * font_size;
  }
  return registry->SelectorFor(std::string(family))->TextWidth(text, font_size);
}

// Width of the widest space-separated word in |text| (the min-content width).
float WidestWordWidth(const graphics::FontRegistry* registry, std::string_view family,
                      std::string_view text, float font_size) {
  float widest = 0;
  std::size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() && IsWordBreak(text[start])) {
      ++start;
    }
    if (start >= text.size()) {
      break;
    }
    const std::size_t end = text.find_first_of(" \t\n\r", start);
    const std::string_view word = text.substr(
        start, end == std::string_view::npos ? text.size() - start : end - start);
    widest = std::max(widest, MeasureTextWidth(registry, family, word, font_size));
    start = end == std::string_view::npos ? text.size() : end;
  }
  return widest;
}

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
  if (child_element.tag_name() == "br") {
    // <br> is a void inline element: it forces a line break and carries the
    // line-height of its style but no text.
    items.push_back(InlineItem{{}, &child_style, &child_element, /*line_break=*/true});
    return;
  }
  for (dom::Node* child : node.ChildNodes()) {
    CollectInline(*child, child_style, &child_element, styles, items);
  }
}

// Breaks inline items into wrapped lines and fills |out_lines|.  Positions are
// relative to the box (origin_x/origin_y are the content box origin).  Word
// widths come from |registry| when provided (real advances with per-character
// font fallback); otherwise the monospace fallback is used.
void LayoutLines(const std::vector<InlineItem>& items, float available_width, float origin_x,
                 float origin_y, const graphics::FontRegistry* registry,
                 std::vector<Line>& out_lines, float& total_height) {
  Line line;
  float x = 0;
  float line_top = 0;

  auto flush_line = [&]() {
    if (line.runs.empty() && line.height <= 0) {
      // Nothing to emit: no content and no height (e.g. leading whitespace).
      return;
    }
    if (!line.runs.empty()) {
      // Position runs vertically within the line.
      for (TextRun& run : line.runs) {
        run.y = origin_y + line_top + (line.height - run.font_size) / 2.0f;
        run.x = origin_x + run.x;
      }
    }
    // Empty lines (line.height > 0, no runs) are emitted too: they are the
    // lines produced by <br>.
    out_lines.push_back(std::move(line));
    line = Line{};
    total_height += out_lines.back().height;
    line_top += out_lines.back().height;
    x = 0;
  };

  auto add_word = [&](std::string_view word, const style::ComputedStyle& style,
                      const dom::Element* element, const graphics::FontSelector* selector) {
    const float word_width =
        selector != nullptr ? selector->TextWidth(word, style.font_size)
                            : static_cast<float>(word.size()) * style.font_size;
    if (x + word_width > available_width && x > 0) {
      flush_line();
    }
    TextRun run;
    run.text = std::string(word);
    run.font_family = style.font_family;
    run.x = x;
    run.font_size = style.font_size;
    run.width = word_width;
    run.color = style.color.value_or(css::Color{0, 0, 0, 255});
    run.underline = style.text_decoration_underline;
    run.element = element;
    line.runs.push_back(std::move(run));
    line.height = std::max(line.height, style.line_height);
    x += word_width;
  };

  for (const InlineItem& item : items) {
    const style::ComputedStyle& style = *item.style;
    if (item.line_break) {
      // <br>: end the current line (content or a previous empty break line),
      // then start a new empty line that carries the break's line height.
      if (!line.runs.empty() || line.height > 0) {
        flush_line();
      }
      line.height = std::max(line.height, style.line_height);
      continue;
    }
    const graphics::FontSelector* selector =
        registry != nullptr ? registry->SelectorFor(style.font_family) : nullptr;
    std::size_t start = 0;
    const std::string& text = item.text;
    while (start < text.size()) {
      while (start < text.size() && IsWordBreak(text[start])) {
        const float space_width =
            selector != nullptr ? selector->Advance(' ', style.font_size)
                                : style.font_size * 0.5f;
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
      add_word(word, style, item.element, selector);
      start = end == std::string::npos ? text.size() : end;
    }
  }
  flush_line();
}

// ---------------------------------------------------------------------------
// Table layout support
// ---------------------------------------------------------------------------

// Intrinsic content width of a box (CSS2.1 "preferred width").
struct IntrinsicWidths {
  float min = 0;  // widest unbreakable unit (word / replaced element)
  float max = 0;  // widest line when laid out on a single line
};

// An element's left+right padding and border (added to intrinsic content).
float HorizontalExtras(const style::ComputedStyle& style) {
  return style.padding_left.value + style.padding_right.value + style.border_left.value +
         style.border_right.value;
}

// Measures the min/max intrinsic content width of |element|, recursing through
// the inline + block content model.  Replaced elements (img/input/...) use
// their explicit width (zero when auto).  Percentages, floats and positioning
// are out of scope for this measurement.  Text is measured through |registry|
// (real advances with per-character fallback) when provided, else with the
// monospace fallback.
IntrinsicWidths MeasureContent(const dom::Element& element, const style::StyleEngine& styles,
                               const graphics::FontRegistry* registry) {
  const style::ComputedStyle& style = styles.StyleFor(element);
  const bool replaced = element.tag_name() == "img" || element.tag_name() == "input" ||
                        element.tag_name() == "textarea" || element.tag_name() == "select";
  if (replaced) {
    IntrinsicWidths w;
    if (style.width.has_value() && !style.width.value().percent) {
      w.min = w.max = style.width.value().value;
    }
    const float extras = HorizontalExtras(style);
    w.min += extras;
    w.max += extras;
    return w;
  }

  IntrinsicWidths out;
  float line_min = 0;
  float line_max = 0;
  auto flush_line = [&]() {
    out.min = std::max(out.min, line_min);
    out.max = std::max(out.max, line_max);
    line_min = 0;
    line_max = 0;
  };

  for (const dom::Node* child : element.ChildNodes()) {
    if (child->node_type() == dom::NodeType::kText) {
      const std::string& text = static_cast<const dom::Text&>(*child).data();
      line_min = std::max(line_min, WidestWordWidth(registry, style.font_family, text,
                                                    style.font_size));
      line_max += MeasureTextWidth(registry, style.font_family, text, style.font_size);
      continue;
    }
    if (child->node_type() != dom::NodeType::kElement) {
      continue;
    }
    const dom::Element& child_el = static_cast<const dom::Element&>(*child);
    const style::ComputedStyle& child_style = styles.StyleFor(child_el);
    if (child_style.display == style::Display::kNone) {
      continue;
    }
    if (child_el.tag_name() == "br") {
      // <br> ends the current line: max-content is the widest segment.
      flush_line();
      continue;
    }
    const bool block_level = child_style.display == style::Display::kBlock ||
                             child_style.display == style::Display::kTable ||
                             child_style.display == style::Display::kTableRowGroup ||
                             child_style.display == style::Display::kTableRow ||
                             child_style.display == style::Display::kTableCell ||
                             child_style.display == style::Display::kTableCaption;
    if (block_level) {
      // Block-level content starts a new line.
      flush_line();
      const IntrinsicWidths cw = MeasureContent(child_el, styles, registry);
      out.min = std::max(out.min, cw.min);
      out.max = std::max(out.max, cw.max);
    } else {
      // Inline content flows onto the current line.
      const IntrinsicWidths cw = MeasureContent(child_el, styles, registry);
      line_min = std::max(line_min, cw.min);
      line_max += cw.max;
    }
  }
  flush_line();
  const float extras = HorizontalExtras(style);
  out.min += extras;
  out.max += extras;
  return out;
}

// One table cell (td/th) with its grid coordinates and laid-out box.
struct CellInfo {
  dom::Element* element = nullptr;
  style::ComputedStyle style;
  int row = 0;
  int col = 0;
  int colspan = 1;
  int rowspan = 1;
  bool grows_downward = false;  // rowspan="0": spans to the end of the row group
  float min_width = 0;
  float max_width = 0;
  std::unique_ptr<LayoutBox> box;
};

bool IsAsciiWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

// Parses an HTML attribute value as a "non-negative integer" (WHATWG HTML
// common-microsyntaxes): leading ASCII whitespace is skipped, then a run of
// digits is read (trailing non-digits are ignored); anything else before the
// first digit is an error.  Returns nullopt when the attribute is absent or the
// value is not a valid non-negative integer.
std::optional<std::int64_t> ParseNonNegativeInt(const dom::Element& element,
                                                std::string_view name) {
  const std::optional<std::string_view> attr = element.GetAttribute(name);
  if (!attr.has_value()) {
    return std::nullopt;
  }
  const std::string_view value = *attr;
  std::size_t i = 0;
  while (i < value.size() && IsAsciiWhitespace(value[i])) {
    ++i;
  }
  if (i >= value.size() || value[i] < '0' || value[i] > '9') {
    return std::nullopt;
  }
  std::int64_t result = 0;
  for (; i < value.size(); ++i) {
    const char c = value[i];
    if (c < '0' || c > '9') {
      break;
    }
    result = result * 10 + (c - '0');
    if (result > 65535) {
      return 65535;  // callers clamp to <= 1000 / <= 65534 anyway
    }
  }
  return result;
}

// Resolves a colspan attribute per the table model: parse as a non-negative
// integer; zero / failure / absent yields 1; values above 1000 clamp to 1000.
int ResolveColspan(const dom::Element& element) {
  const std::optional<std::int64_t> v = ParseNonNegativeInt(element, "colspan");
  if (!v.has_value() || *v == 0) {
    return 1;
  }
  return static_cast<int>(std::min<std::int64_t>(*v, 1000));
}

// Resolves a rowspan attribute per the table model: parse as a non-negative
// integer; failure / absent yields 1; values above 65534 clamp to 65534.  Sets
// |grows_downward| when the value is zero, meaning the cell spans the remaining
// rows of its row group.
int ResolveRowspan(const dom::Element& element, bool& grows_downward) {
  const std::optional<std::int64_t> v = ParseNonNegativeInt(element, "rowspan");
  grows_downward = false;
  if (!v.has_value()) {
    return 1;
  }
  if (*v == 0) {
    grows_downward = true;
    return 1;
  }
  return static_cast<int>(std::min<std::int64_t>(*v, 65534));
}

// Shifts a laid-out box (and its descendants and text runs) by (dx, dy).  Used
// to move a cell's content from its local (0,0) origin into its grid slot.
void TranslateBox(LayoutBox& box, float dx, float dy) {
  box.x += dx;
  box.y += dy;
  for (Line& line : box.lines) {
    for (TextRun& run : line.runs) {
      run.x += dx;
      run.y += dy;
    }
  }
  for (auto& child : box.children) {
    TranslateBox(*child, dx, dy);
  }
}

// Resolves per-column content widths for a table.  Columns carrying a cell
// with an explicit width are fixed; the remaining table width is distributed
// across auto columns proportionally to their measured max-content width.
std::vector<float> ComputeColumnWidths(const std::vector<CellInfo>& cells, int ncols,
                                       float table_width) {
  const std::size_t n = static_cast<std::size_t>(std::max(ncols, 0));
  std::vector<float> fixed(n, -1.0f);
  std::vector<float> auto_max(n, 0.0f);
  for (const CellInfo& cell : cells) {
    if (cell.colspan != 1) {
      continue;  // a spanning cell's width is the sum of its columns
    }
    const std::size_t c = static_cast<std::size_t>(cell.col);
    if (cell.style.width.has_value()) {
      const float w = cell.style.width.value().percent
                          ? cell.style.width.value().value / 100.0f * table_width
                          : cell.style.width.value().value;
      fixed[c] = std::max(fixed[c], w);
    } else {
      auto_max[c] = std::max(auto_max[c], cell.max_width);
    }
  }
  float fixed_sum = 0;
  for (const float w : fixed) {
    if (w >= 0) {
      fixed_sum += w;
    }
  }
  const float remainder = std::max(0.0f, table_width - fixed_sum);
  float auto_total = 0;
  std::size_t auto_count = 0;
  for (std::size_t c = 0; c < n; ++c) {
    if (fixed[c] < 0) {
      auto_total += auto_max[c];
      ++auto_count;
    }
  }
  std::vector<float> widths(n, 0.0f);
  for (std::size_t c = 0; c < n; ++c) {
    if (fixed[c] >= 0) {
      widths[c] = fixed[c];
    } else if (auto_count > 0) {
      widths[c] = auto_total > 0 ? remainder * auto_max[c] / auto_total
                                 : remainder / static_cast<float>(auto_count);
    }
  }
  return widths;
}

// Sums |count| column widths starting at |start| (clamped to the column set).
float SumColumns(const std::vector<float>& widths, int start, int count) {
  float sum = 0;
  const std::size_t begin = static_cast<std::size_t>(std::max(start, 0));
  const std::size_t end = std::min(begin + static_cast<std::size_t>(std::max(count, 0)),
                                   widths.size());
  for (std::size_t i = begin; i < end; ++i) {
    sum += widths[i];
  }
  return sum;
}

}  // namespace

std::unique_ptr<LayoutBox> LayoutEngine::BuildLayoutTree(dom::Document& document,
                                                         float viewport_width) {
  // Coordinates are absolute (viewport space).  BuildBlock lays out |element|
  // inside a containing block whose content box starts at |origin_x|/|origin_y|.
  struct Builder {
    const style::StyleEngine& styles;
    const graphics::FontRegistry* registry;  // may be null (monospace fallback)

    // Resolves the box-model edges (margins, borders, padding) of |box|.
    void ResolveBoxEdges(LayoutBox& box, float containing_width) {
      box.margin_top = ResolveSize(box.style.margin_top, containing_width);
      box.margin_right = ResolveSize(box.style.margin_right, containing_width);
      box.margin_bottom = ResolveSize(box.style.margin_bottom, containing_width);
      box.margin_left = ResolveSize(box.style.margin_left, containing_width);
      box.border_top = ResolveSize(box.style.border_top, containing_width);
      box.border_right = ResolveSize(box.style.border_right, containing_width);
      box.border_bottom = ResolveSize(box.style.border_bottom, containing_width);
      box.border_left = ResolveSize(box.style.border_left, containing_width);
      box.padding_top = ResolveSize(box.style.padding_top, containing_width);
      box.padding_right = ResolveSize(box.style.padding_right, containing_width);
      box.padding_bottom = ResolveSize(box.style.padding_bottom, containing_width);
      box.padding_left = ResolveSize(box.style.padding_left, containing_width);
    }

    // Lays out the block-level children and inline content of |element| into
    // |box| (which already has its width and content origin set).  Fills
    // box.children and box.lines; returns the total content height.
    float LayoutBlockContent(LayoutBox& box, dom::Element& element, float avail_width) {
      float cursor_y = 0;
      std::vector<InlineItem> inline_items;
      for (dom::Node* child : element.ChildNodes()) {
        if (child->node_type() == dom::NodeType::kText) {
          CollectText(static_cast<dom::Text*>(child)->data(), box.style, &element, inline_items);
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
          std::unique_ptr<LayoutBox> child_box = BuildBlock(
              child_element, avail_width, box.content_x(), box.content_y() + cursor_y);
          cursor_y += child_box->margin_top + child_box->height + child_box->margin_bottom;
          box.children.push_back(std::move(child_box));
        } else if (child_style.display == style::Display::kTable) {
          std::unique_ptr<LayoutBox> child_box = BuildTable(
              child_element, avail_width, box.content_x(), box.content_y() + cursor_y);
          cursor_y += child_box->margin_top + child_box->height + child_box->margin_bottom;
          box.children.push_back(std::move(child_box));
        } else {
          // Inline element: its text flows into this box's lines.
          CollectInline(child_element, child_style, &child_element, styles, inline_items);
        }
      }

      float lines_height = 0;
      LayoutLines(inline_items, avail_width, box.content_x(), box.content_y() + cursor_y,
                  registry, box.lines, lines_height);
      return cursor_y + lines_height;
    }

    std::unique_ptr<LayoutBox> BuildBlock(dom::Element& element, float containing_width,
                                          float origin_x, float origin_y) {
      auto box = std::make_unique<LayoutBox>();
      box->element = &element;
      box->style = styles.StyleFor(element);
      ResolveBoxEdges(*box, containing_width);

      // Width: explicit (px or %) or fill the containing block.
      float content_width;
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

      const float avail_width = box->width - box->border_left - box->border_right -
                                box->padding_left - box->padding_right;
      float content_height = LayoutBlockContent(*box, element, avail_width);

      if (box->style.height.has_value() && !box->style.height.value().percent) {
        content_height = std::max(content_height, box->style.height.value().value);
      }
      box->height = content_height + box->border_top + box->border_bottom + box->padding_top +
                    box->padding_bottom;
      return box;
    }

    // Lays out a table cell's content into a box of the given content width.
    // The box is positioned at (0,0); the caller translates it to its grid
    // slot.  Margins are ignored (table cells have no margin in CSS).
    std::unique_ptr<LayoutBox> LayoutCell(dom::Element& element, float content_width) {
      auto box = std::make_unique<LayoutBox>();
      box->element = &element;
      box->style = styles.StyleFor(element);
      ResolveBoxEdges(*box, content_width);
      box->width = content_width + box->border_left + box->border_right + box->padding_left +
                   box->padding_right;
      const float avail_width = box->width - box->border_left - box->border_right -
                                box->padding_left - box->padding_right;
      const float content_height = LayoutBlockContent(*box, element, avail_width);
      box->height = content_height + box->border_top + box->border_bottom + box->padding_top +
                    box->padding_bottom;
      return box;
    }

    std::unique_ptr<LayoutBox> BuildTable(dom::Element& element, float containing_width,
                                          float origin_x, float origin_y) {
      auto table = std::make_unique<LayoutBox>();
      table->element = &element;
      table->style = styles.StyleFor(element);
      ResolveBoxEdges(*table, containing_width);

      // Table width: explicit, or fill the containing block (auto is treated as
      // 100% rather than CSS shrink-to-fit; documented limitation).
      float content_width;
      if (table->style.width.has_value()) {
        content_width = ResolveSize(table->style.width.value(), containing_width);
      } else {
        content_width = containing_width - table->margin_left - table->margin_right -
                        table->border_left - table->border_right - table->padding_left -
                        table->padding_right;
        if (content_width < 0) {
          content_width = 0;
        }
      }
      table->width = content_width + table->border_left + table->border_right +
                     table->padding_left + table->padding_right;
      table->x = origin_x + table->margin_left - table->border_left - table->padding_left;
      table->y = origin_y + table->margin_top - table->border_top - table->padding_top;
      const float table_x = table->content_x();
      const float table_y = table->content_y();

      // Collect rows (flattening row groups) and cells, then place cells into a
      // grid honoring colspan/rowspan.  Row-group boundaries are kept so a
      // rowspan="0" cell spans to the end of its own group (thead/tbody/tfoot,
      // or the implicit group of consecutive anonymous <tr>).
      struct RowInfo {
        dom::Element* element = nullptr;
        style::ComputedStyle style;
      };
      struct RowGroup {
        int start = 0;  // inclusive first row index
        int end = 0;    // exclusive last row index
      };
      std::vector<RowInfo> rows;
      std::vector<RowGroup> row_groups;
      std::vector<CellInfo> cells;
      std::vector<std::vector<int>> grid;  // grid[r][c] = cell index, or -1
      int implicit_group_start_ = -1;  // first row of the current implicit group

      auto flush_implicit_group = [&](int next_row) {
        if (implicit_group_start_ >= 0 && implicit_group_start_ < next_row) {
          row_groups.push_back(RowGroup{implicit_group_start_, next_row});
        }
        implicit_group_start_ = -1;
      };

      auto collect_rows = [&](const dom::Element& container) {
        for (dom::Node* child : container.ChildNodes()) {
          if (child->node_type() != dom::NodeType::kElement) {
            continue;
          }
          dom::Element& el = static_cast<dom::Element&>(*child);
          const style::ComputedStyle& cs = styles.StyleFor(el);
          if (cs.display == style::Display::kTableRowGroup) {
            flush_implicit_group(static_cast<int>(rows.size()));
            const int group_start = static_cast<int>(rows.size());
            for (dom::Node* gchild : el.ChildNodes()) {
              if (gchild->node_type() == dom::NodeType::kElement &&
                  styles.StyleFor(static_cast<dom::Element&>(*gchild)).display ==
                      style::Display::kTableRow) {
                dom::Element& rel = static_cast<dom::Element&>(*gchild);
                rows.push_back(RowInfo{&rel, styles.StyleFor(rel)});
              }
            }
            row_groups.push_back(RowGroup{group_start, static_cast<int>(rows.size())});
          } else if (cs.display == style::Display::kTableRow) {
            // Consecutive anonymous <tr> children form one implicit row group.
            if (implicit_group_start_ < 0) {
              implicit_group_start_ = static_cast<int>(rows.size());
            }
            rows.push_back(RowInfo{&el, cs});
          }
          // caption / colgroup / col / whitespace are not part of the row grid.
        }
      };
      collect_rows(element);
      flush_implicit_group(static_cast<int>(rows.size()));
      const int nrows = static_cast<int>(rows.size());

      // Collect every cell with its (unresolved) span; rowspan="0" is resolved
      // below once the row-group boundaries are known.
      for (int r = 0; r < nrows; ++r) {
        dom::Element& tr = *rows[static_cast<std::size_t>(r)].element;
        for (dom::Node* child : tr.ChildNodes()) {
          if (child->node_type() != dom::NodeType::kElement) {
            continue;
          }
          dom::Element& cel = static_cast<dom::Element&>(*child);
          const style::ComputedStyle& cs = styles.StyleFor(cel);
          if (cs.display != style::Display::kTableCell) {
            continue;
          }
          CellInfo info;
          info.element = &cel;
          info.style = cs;
          info.row = r;
          info.colspan = ResolveColspan(cel);
          info.rowspan = ResolveRowspan(cel, info.grows_downward);
          cells.push_back(std::move(info));
        }
      }
      // rowspan="0": the cell spans the remaining rows of its own row group
      // (WHATWG tables.html: "span all the remaining rows in the row group").
      for (CellInfo& cell : cells) {
        if (!cell.grows_downward) {
          continue;
        }
        int group_end = nrows;
        for (const RowGroup& group : row_groups) {
          if (cell.row >= group.start && cell.row < group.end) {
            group_end = group.end;
            break;
          }
        }
        cell.rowspan = std::max(1, group_end - cell.row);
      }

      // Place cells into the grid: find each cell's first free column, honoring
      // the (now resolved) colspan/rowspan occupancy.
      for (std::size_t i = 0; i < cells.size(); ++i) {
        CellInfo& info = cells[i];
        const std::size_t ri = static_cast<std::size_t>(info.row);
        if (grid.size() <= ri) {
          grid.resize(ri + 1);
        }
        int col = 0;
        while (true) {
          bool free = true;
          for (int cc = col; cc < col + info.colspan; ++cc) {
            const std::size_t cci = static_cast<std::size_t>(cc);
            if (cci < grid[ri].size() && grid[ri][cci] != -1) {
              free = false;
              break;
            }
          }
          if (free) {
            break;
          }
          ++col;
        }
        info.col = col;
        for (int rr = info.row; rr < info.row + info.rowspan; ++rr) {
          const std::size_t rri = static_cast<std::size_t>(rr);
          if (grid.size() <= rri) {
            grid.resize(rri + 1);
          }
          const std::size_t cols = static_cast<std::size_t>(col + info.colspan);
          if (grid[rri].size() < cols) {
            grid[rri].resize(cols, -1);
          }
          for (int cc = col; cc < col + info.colspan; ++cc) {
            grid[rri][static_cast<std::size_t>(cc)] = static_cast<int>(i);
          }
        }
      }

      const int ncols = grid.empty()
                            ? 0
                            : static_cast<int>(std::max_element(
                                                  grid.begin(), grid.end(),
                                                  [](const auto& a, const auto& b) {
                                                    return a.size() < b.size();
                                                  })
                                                  ->size());

      // Intrinsic widths for auto column sizing.
      for (CellInfo& cell : cells) {
        const IntrinsicWidths w = MeasureContent(*cell.element, styles, registry);
        cell.min_width = w.min;
        cell.max_width = w.max;
      }

      const std::vector<float> col_widths = ComputeColumnWidths(cells, ncols, content_width);

      // Lay out each cell (at a local origin) and derive row heights.
      for (CellInfo& cell : cells) {
        cell.box = LayoutCell(*cell.element, SumColumns(col_widths, cell.col, cell.colspan));
      }

      std::vector<float> row_heights(rows.size(), 0.0f);
      for (const CellInfo& cell : cells) {
        if (cell.rowspan == 1) {
          const std::size_t ri = static_cast<std::size_t>(cell.row);
          row_heights[ri] = std::max(row_heights[ri], cell.box->height);
        }
      }
      // Row-spanning cells: make sure the spanned rows are tall enough, giving
      // any overflow to the last spanned row (simplified CSS2.1 distribution).
      for (const CellInfo& cell : cells) {
        if (cell.rowspan <= 1) {
          continue;
        }
        float spanned = 0;
        for (int r = cell.row;
             r < cell.row + cell.rowspan && r < static_cast<int>(row_heights.size()); ++r) {
          spanned += row_heights[static_cast<std::size_t>(r)];
        }
        if (cell.box->height > spanned) {
          const int last = std::min(cell.row + cell.rowspan - 1,
                                    static_cast<int>(row_heights.size()) - 1);
          row_heights[static_cast<std::size_t>(last)] += cell.box->height - spanned;
        }
      }

      // Build the box tree (table -> rows -> cells) with grid positions.
      float y = 0;
      for (std::size_t r = 0; r < rows.size(); ++r) {
        auto row_box = std::make_unique<LayoutBox>();
        row_box->element = rows[r].element;
        row_box->style = rows[r].style;
        row_box->x = table_x;
        row_box->y = table_y + y;
        row_box->width = content_width;
        row_box->height = row_heights[r];
        for (CellInfo& cell : cells) {
          if (cell.row != static_cast<int>(r)) {
            continue;
          }
          const float cx = table_x + SumColumns(col_widths, 0, cell.col);
          const float cy = table_y + y;
          TranslateBox(*cell.box, cx, cy);
          if (cell.rowspan <= 1) {
            cell.box->height = row_heights[r];
          } else {
            float spanned = 0;
            for (int rr = cell.row;
                 rr < cell.row + cell.rowspan && rr < static_cast<int>(row_heights.size());
                 ++rr) {
              spanned += row_heights[static_cast<std::size_t>(rr)];
            }
            cell.box->height = spanned;
          }
          row_box->children.push_back(std::move(cell.box));
        }
        table->children.push_back(std::move(row_box));
        y += row_heights[r];
      }

      table->height = y + table->border_top + table->border_bottom + table->padding_top +
                      table->padding_bottom;
      return table;
    }
  };

  dom::Element* root = document.document_element();
  if (root == nullptr) {
    return nullptr;
  }
  Builder builder{styles_, registry_};
  return builder.BuildBlock(*root, viewport_width, 0, 0);
}

}  // namespace neko::layout
