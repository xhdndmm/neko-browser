#include "neko/paint/painter.h"

#include "neko/image/image.h"
#include "neko/paint/display_list.h"

#include <algorithm>
#include <cmath>

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

// Collects every positioned (absolute/fixed) descendant in tree order.  They
// all paint after the in-flow content (there is no z-index support yet, so
// they are treated as z-index:auto and keep tree order).
void CollectPositioned(const layout::LayoutBox& box, std::vector<const layout::LayoutBox*>& out)
{
  for (const auto& child : box.positioned_children) {
    out.push_back(child.get());
    CollectPositioned(*child, out);
  }
  for (const auto& child : box.children) {
    CollectPositioned(*child, out);
  }
  for (const auto& f : box.floats) {
    CollectPositioned(*f, out);
  }
  for (const auto& line : box.lines) {
    for (const auto& ib : line.boxes) {
      if (ib.block_box != nullptr) {
        CollectPositioned(*ib.block_box, out);
      }
    }
  }
}

} // namespace

DisplayList Painter::Paint() const
{
  DisplayList list;
  if (root_ == nullptr) {
    return list;
  }
  // CSS 2.1 Appendix E painting order (simplified): paint the in-flow content
  // (block backgrounds, floats, inline content) first, then every positioned
  // descendant on top.  Painting positioned children inside their own box let
  // a later in-flow sibling cover them — an absolutely-positioned dropdown
  // menu was hidden behind the page content below the header.
  PaintBox(*root_, list);
  std::vector<const layout::LayoutBox*> positioned;
  CollectPositioned(*root_, positioned);
  // z-index order (CSS 2.1 §9.9): auto/0 first, then ascending; stable, so an
  // equal z-index keeps tree order.  A z-index:100 dropdown menu therefore
  // paints above an overlaying banner that uses z-index:auto.
  std::stable_sort(positioned.begin(),
                   positioned.end(),
                   [](const layout::LayoutBox* a, const layout::LayoutBox* b) {
                     return a->style.z_index.value_or(0) < b->style.z_index.value_or(0);
                   });
  for (const layout::LayoutBox* box : positioned) {
    PaintBox(*box, list);
  }
  return list;
}

void Painter::PaintBox(const layout::LayoutBox& box, DisplayList& list) const
{
  // visibility: hidden suppresses the box's own decorations and content but
  // keeps its layout space; descendants that declare visibility: visible
  // again still paint (CSS 2.2 §11.2), so the child recursion below is not
  // gated here.
  const bool visible = box.style.visibility != style::Visibility::kHidden;
  if (visible) {
    PaintBoxSelf(box, list);
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
      } else if (inline_box.image != nullptr && !inline_box.image->empty() &&
                 inline_box.style.visibility != style::Visibility::kHidden) {
        list.DrawImage(inline_box.x,
                       inline_box.y,
                       inline_box.width,
                       inline_box.height,
                       *inline_box.image,
                       inline_box.style.object_fit);
      }
    }
  }

  // Inline element backgrounds (CSS 2.2 §8.4.1): fill behind each group of
  // consecutive runs that share a source element carrying a background colour
  // (e.g. a padded `background:#222` overlay link).  Runs whose source is the
  // block itself duplicate the block's own background, which PaintBox already
  // filled, so they are skipped.
  for (const layout::Line& line : box.lines) {
    for (std::size_t i = 0; i < line.runs.size();) {
      const layout::TextRun& first = line.runs[i];
      if (!first.visible || first.background.a == 0 || first.element == nullptr ||
          first.element == box.element) {
        ++i;
        continue;
      }
      const dom::Element* element = first.element;
      const std::uint8_t alpha = first.background.a;
      float left = first.x - first.padding_left;
      float right = first.x + first.width + first.padding_right;
      std::size_t j = i + 1;
      while (j < line.runs.size() && line.runs[j].element == element &&
             line.runs[j].background.a == alpha) {
        right = line.runs[j].x + line.runs[j].width + line.runs[j].padding_right;
        ++j;
      }
      const float top = first.y - line.baseline_offset - first.padding_top;
      const float height = line.height + first.padding_top + first.padding_bottom;
      if (right > left && height > 0.0f) {
        list.FillRect(left, top, right - left, height, first.background);
      }
      i = j;
    }
  }

  // Inline text.
  for (const layout::Line& line : box.lines) {
    for (const layout::TextRun& run : line.runs) {
      if (run.visible && !run.text.empty()) {
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

  if (clips) {
    list.PopClip();
  }
}

void Painter::PaintBoxSelf(const layout::LayoutBox& box, DisplayList& list) const
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
    // wallpaper).  A missing or not-yet-decoded image is skipped.
    if (!box.background_image_url.empty() && box.background_image != nullptr &&
        !box.background_image->empty()) {
      const image::Image& bg_image = *box.background_image;
      const bool repeat_x = box.style.background_repeat_x;
      const bool repeat_y = box.style.background_repeat_y;
      if (!repeat_x && !repeat_y) {
        // no-repeat keeps the previous cover draw (a background-size: cover
        // approximation that wallpaper-style pages rely on).
        list.DrawImage(box.x, box.y, box.width, box.height, bg_image, style::ObjectFit::kCover);
      } else {
        // Tiled background (CSS Backgrounds 3 §2.4): natural-size tiles,
        // positioned by the keyword fractions, clipped to the border box.
        // news.cctv.com tiles 1px-wide gradient strips (repeat-x) as its
        // section backdrops; drawing one pixel with cover painted whole
        // sections a flat smeared color and buried the content.
        const float tile_w = static_cast<float>(bg_image.width);
        const float tile_h = static_cast<float>(bg_image.height);
        // A 1px axis repeated along itself is identical to one stretched
        // draw (every column/row of the source is the same); collapse it so
        // a 1px strip does not emit one command per pixel of box width.
        const bool stretch_x = repeat_x && bg_image.width == 1;
        const bool stretch_y = repeat_y && bg_image.height == 1;
        constexpr int kMaxTilesPerAxis = 4096; // guard for huge pages
        const float phase_x =
            stretch_x ? 0.0F : (box.width - tile_w) * box.style.background_position_x;
        const float phase_y =
            stretch_y ? 0.0F : (box.height - tile_h) * box.style.background_position_y;
        // Repeated axes extend the tile grid backwards so the strip before a
        // positive phase is covered too (CSS paints the whole area, the
        // position only sets the pattern phase); a non-repeated axis keeps
        // its exact position.
        const float start_x =
            repeat_x && phase_x > 0 ? phase_x - std::ceil(phase_x / tile_w) * tile_w : phase_x;
        const float start_y =
            repeat_y && phase_y > 0 ? phase_y - std::ceil(phase_y / tile_h) * tile_h : phase_y;
        const int cols =
            stretch_x  ? 1
            : repeat_x ? std::clamp(static_cast<int>(std::ceil((box.width - start_x) / tile_w)),
                                    1,
                                    kMaxTilesPerAxis)
                       : 1;
        const int rows =
            stretch_y  ? 1
            : repeat_y ? std::clamp(static_cast<int>(std::ceil((box.height - start_y) / tile_h)),
                                    1,
                                    kMaxTilesPerAxis)
                       : 1;
        const float draw_w = stretch_x ? box.width : tile_w;
        const float draw_h = stretch_y ? box.height : tile_h;
        list.PushClip(box.x, box.y, box.width, box.height);
        for (int row = 0; row < rows; ++row) {
          for (int col = 0; col < cols; ++col) {
            list.DrawImage(box.x + start_x + static_cast<float>(col) * tile_w,
                           box.y + start_y + static_cast<float>(row) * tile_h,
                           draw_w,
                           draw_h,
                           bg_image,
                           style::ObjectFit::kFill);
          }
        }
        list.PopClip();
      }
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
}

} // namespace neko::paint
