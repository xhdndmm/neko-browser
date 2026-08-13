#include "neko/paint/painter.h"

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

  // Inline text.
  for (const layout::Line& line : box.lines) {
    for (const layout::TextRun& run : line.runs) {
      if (!run.text.empty()) {
        list.DrawText(run.x, run.y, run.text, run.font_size, run.color, run.underline);
      }
    }
  }

  // Block children (back to front).
  for (const auto& child : box.children) {
    PaintBox(*child, list);
  }
}

}  // namespace neko::paint
