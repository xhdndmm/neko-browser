#include "neko/paint/painter.h"

#include "neko/image/image.h"
#include "neko/paint/display_list.h"

namespace neko::paint {

DisplayList Painter::Paint() const {
  DisplayList list;
  if (root_ != nullptr) {
    PaintBox(*root_, list);
  }
  return list;
}

void Painter::PaintBox(const layout::LayoutBox& box, DisplayList& list) const {
  // Background covers the border box.
  if (box.style.background_color.has_value()) {
    list.FillRect(box.x, box.y, box.width, box.height, box.style.background_color.value());
  }

  // Border.
  if (box.border_top > 0 || box.border_right > 0 || box.border_bottom > 0 || box.border_left > 0) {
    const css::Color border_color = box.style.border_color.value_or(css::Color{0, 0, 0, 255});
    list.BorderRect(box.x, box.y, box.width, box.height, box.border_top, box.border_right,
                    box.border_bottom, box.border_left, border_color);
  }

  // Replaced content: the decoded image drawn into the content box per
  // object-fit.
  if (box.image != nullptr && !box.image->empty()) {
    list.DrawImage(box.content_x(), box.content_y(), box.content_width(), box.content_height(),
                   *box.image, box.style.object_fit);
  }

  // Atomic inline boxes within lines.
  for (const layout::Line& line : box.lines) {
    for (const layout::InlineBox& inline_box : line.boxes) {
      if (inline_box.block_box != nullptr) {
        // An inline-block: paint its inner block layout (background, border,
        // content and children).  It carries the element's own style.
        PaintBox(*inline_box.block_box, list);
      } else if (inline_box.image != nullptr && !inline_box.image->empty()) {
        list.DrawImage(inline_box.x, inline_box.y, inline_box.width, inline_box.height,
                       *inline_box.image, inline_box.style.object_fit);
      }
    }
  }

  // Inline text.
  for (const layout::Line& line : box.lines) {
    for (const layout::TextRun& run : line.runs) {
      if (!run.text.empty()) {
        list.DrawText(run.x, run.y, run.text, run.font_size, run.color, run.underline,
                      run.font_family, run.font_weight, run.font_italic);
      }
    }
  }

  // Block children (back to front).
  for (const auto& child : box.children) {
    PaintBox(*child, list);
  }

  // Absolutely positioned descendants paint above in-flow content.
  for (const auto& child : box.positioned_children) {
    PaintBox(*child, list);
  }
}

}  // namespace neko::paint
