#include "neko/paint/painter.h"

#include "neko/image/image.h"
#include "neko/paint/display_list.h"

namespace neko::paint {
namespace {

// Native button palette: an approximation of the platform buttonface look
// (the engine has no platform widget rendering).
constexpr css::Color kButtonFace{0xec, 0xec, 0xec, 255};
constexpr css::Color kButtonBorderLight{0xff, 0xff, 0xff, 255};
constexpr css::Color kButtonBorderDark{0x8a, 0x8a, 0x8a, 255};

// True when |box| must be painted with the native button appearance
// (CSS-UI-4 §7.2 + WHATWG rendering §15.5.4): appearance:auto gives the
// <button> element its native look; appearance:button forces one on any
// element; appearance:none disables it so author background/border apply.
bool HasNativeButtonAppearance(const layout::LayoutBox& box)
{
  if (box.style.appearance == style::Appearance::kButton) {
    return true;
  }
  if (box.style.appearance == style::Appearance::kAuto) {
    return box.element != nullptr && box.element->tag_name() == "button";
  }
  return false;
}

// Resolves the border-radius against the box width (a percentage is relative
// to the box's dimensions; a single-radius model uses the width for both
// axes, which is exact for squares and a documented approximation otherwise).
float ResolveBorderRadius(const style::SizeSpec& spec, float box_width)
{
  if (spec.percent) {
    return box_width * spec.value / 100.0f;
  }
  return spec.value;
}

// Fills a box's border box with its background color, honouring border-radius.
void FillBackground(const layout::LayoutBox& box, css::Color color, DisplayList& list)
{
  if (box.style.border_radius.has_value()) {
    const float radius = ResolveBorderRadius(box.style.border_radius.value(), box.width);
    list.FillRoundRect(box.x, box.y, box.width, box.height, radius, color);
  } else {
    list.FillRect(box.x, box.y, box.width, box.height, color);
  }
}

} // namespace

DisplayList Painter::Paint() const
{
  DisplayList list;
  if (root_ != nullptr) {
    PaintBox(*root_, list);
  }
  return list;
}

void Painter::PaintBox(const layout::LayoutBox& box, DisplayList& list) const
{
  if (HasNativeButtonAppearance(box)) {
    // Native button look: buttonface background + outset border as the
    // default decorations.  Author background-color/border-color
    // declarations take precedence over the native face (matching browser
    // behavior — CSS-UI-4 §7.2.3 lets a UA ignore them, but engines keep
    // author styles, e.g. `background:#2a3c54` dropdown buttons).  Border
    // widths come from layout so the content box stays consistent.
    if (box.style.background_color.has_value()) {
      FillBackground(box, box.style.background_color.value(), list);
    } else {
      FillBackground(box, kButtonFace, list);
    }
    if (box.style.border_color.has_value()) {
      list.BorderRect(box.x,
                      box.y,
                      box.width,
                      box.height,
                      box.border_top,
                      box.border_right,
                      box.border_bottom,
                      box.border_left,
                      box.style.border_color.value());
    } else if (box.border_top > 0 || box.border_right > 0 || box.border_bottom > 0 ||
               box.border_left > 0) {
      // Outset border: light top/left, dark bottom/right.
      list.BorderRect(box.x,
                      box.y,
                      box.width,
                      box.height,
                      box.border_top,
                      0,
                      0,
                      box.border_left,
                      kButtonBorderLight);
      list.BorderRect(box.x,
                      box.y,
                      box.width,
                      box.height,
                      0,
                      box.border_right,
                      box.border_bottom,
                      0,
                      kButtonBorderDark);
    }
  } else {
    // Background covers the border box.
    if (box.style.background_color.has_value()) {
      FillBackground(box, box.style.background_color.value(), list);
    }

    // CSS background-image paints over the background color (e.g. Bing's
    // wallpaper).  A missing or not-yet-decoded image is skipped; cover keeps
    // the image filling the box without distortion.
    if (!box.background_image_url.empty() && box.background_image != nullptr &&
        !box.background_image->empty()) {
      list.DrawImage(
          box.x, box.y, box.width, box.height, *box.background_image, style::ObjectFit::kCover);
    }

    // Border.
    if (box.border_top > 0 || box.border_right > 0 || box.border_bottom > 0 ||
        box.border_left > 0) {
      const css::Color border_color = box.style.border_color.value_or(css::Color{0, 0, 0, 255});
      list.BorderRect(box.x,
                      box.y,
                      box.width,
                      box.height,
                      box.border_top,
                      box.border_right,
                      box.border_bottom,
                      box.border_left,
                      border_color);
    }
  }

  // Replaced content: the decoded image drawn into the content box per
  // object-fit.
  if (box.image != nullptr && !box.image->empty()) {
    list.DrawImage(box.content_x(),
                   box.content_y(),
                   box.content_width(),
                   box.content_height(),
                   *box.image,
                   box.style.object_fit);
  }

  // overflow: hidden/auto/scroll clips the box's content (and its
  // descendants) to the padding box.  The box's own background/border paint
  // above the clip boundary and are NOT clipped.
  const bool clips = box.style.overflow != style::Overflow::kVisible;
  if (clips) {
    list.PushClip(box.content_x(), box.content_y(), box.content_width(), box.content_height());
  }

  // Block children (back to front).
  for (const auto& child : box.children) {
    PaintBox(*child, list);
  }

  // Floats paint above in-flow block children but below inline content
  // (CSS2.1 Appendix E), so a float's background/text is not covered by a
  // later block-level sibling.
  for (const auto& f : box.floats) {
    PaintBox(*f, list);
  }

  // Atomic inline boxes within lines.
  for (const layout::Line& line : box.lines) {
    for (const layout::InlineBox& inline_box : line.boxes) {
      if (inline_box.block_box != nullptr) {
        // An inline-block: paint its inner block layout (background, border,
        // content and children).  It carries the element's own style.
        PaintBox(*inline_box.block_box, list);
      } else if (inline_box.image != nullptr && !inline_box.image->empty()) {
        list.DrawImage(inline_box.x,
                       inline_box.y,
                       inline_box.width,
                       inline_box.height,
                       *inline_box.image,
                       inline_box.style.object_fit);
      }
    }
  }

  // Inline text.
  for (const layout::Line& line : box.lines) {
    for (const layout::TextRun& run : line.runs) {
      if (!run.text.empty()) {
        list.DrawText(run.x,
                      run.y,
                      run.text,
                      run.font_size,
                      run.color,
                      run.underline,
                      run.font_family,
                      run.font_weight,
                      run.font_italic);
      }
    }
  }

  // Absolutely positioned descendants paint above in-flow content.
  for (const auto& child : box.positioned_children) {
    PaintBox(*child, list);
  }

  if (clips) {
    list.PopClip();
  }
}

} // namespace neko::paint
