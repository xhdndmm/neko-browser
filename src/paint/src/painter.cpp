#include "neko/paint/painter.h"

#include "neko/image/image.h"
#include "neko/paint/display_list.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

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

// Native widget innards (WHATWG §15.5, paint layer "Wave 2"): the checkbox
// tick, the radio dot and the drop-down arrow, drawn over the control's
// chrome.  The marks are built from axis-aligned fills (plus one rounded
// fill for the dot) so they are deterministic across fonts and platforms --
// no glyph or platform widget is involved.  A disabled control draws them in
// gray.  Still NOT IMPLEMENTED: the range slider track/thumb and the colour
// well's swatch (the range and colour-well value text itself comes from the
// layout text run).
void PaintWidget(const layout::LayoutBox& box, DisplayList& list)
{
  const css::Color ink = box.form_control_disabled ? css::Color{0x80, 0x80, 0x80, 255}
                                                   : css::Color{0x00, 0x00, 0x00, 255};
  // Widget marks snap to whole pixels: an inline-block origin is fractional
  // (baseline alignment), and the rasterizer fills integer spans -- an
  // un-snapped 1px row would expand into two rows and blur the mark.
  const auto snap = [](float v) { return std::round(v); };
  switch (box.form_control) {
  case layout::FormControlKind::kCheckbox: {
    if (!box.form_control_checked) {
      break;
    }
    // The tick as a literal 9x9 bitmap (the classic checkmark), one draw
    // command per ink run, centred in the content box.  Literal bitmaps stay
    // exact at any DPR and any content size.
    static constexpr const char* kTick[9] = {
        ".........",
        "......###",
        ".....###.",
        ".##..##..",
        "..####...",
        "...##....",
        ".........",
        ".........",
        ".........",
    };
    const float ox = snap(box.content_x() + (box.content_width() - 9.0f) / 2.0f);
    const float oy = snap(box.content_y() + (box.content_height() - 9.0f) / 2.0f);
    for (int row = 0; row < 9; ++row) {
      for (int col = 0; col < 9;) {
        if (kTick[row][col] != '#') {
          ++col;
          continue;
        }
        int run = 1;
        while (col + run < 9 && kTick[row][col + run] == '#') {
          ++run;
        }
        list.FillRect(ox + static_cast<float>(col),
                      oy + static_cast<float>(row),
                      static_cast<float>(run),
                      1,
                      ink);
        col += run;
      }
    }
    break;
  }
  case layout::FormControlKind::kRadio: {
    if (!box.form_control_checked) {
      break;
    }
    // The dot as a literal 7x7 circle bitmap, centred in the content box.
    static constexpr const char* kDot[7] = {
        "..###..",
        ".#####.",
        "#######",
        "#######",
        "#######",
        ".#####.",
        "..###..",
    };
    const float ox = snap(box.content_x() + (box.content_width() - 7.0f) / 2.0f);
    const float oy = snap(box.content_y() + (box.content_height() - 7.0f) / 2.0f);
    for (int row = 0; row < 7; ++row) {
      for (int col = 0; col < 7;) {
        if (kDot[row][col] != '#') {
          ++col;
          continue;
        }
        int run = 1;
        while (col + run < 7 && kDot[row][col + run] == '#') {
          ++run;
        }
        list.FillRect(ox + static_cast<float>(col),
                      oy + static_cast<float>(row),
                      static_cast<float>(run),
                      1,
                      ink);
        col += run;
      }
    }
    break;
  }
  case layout::FormControlKind::kSelect: {
    // A small downward triangle near the trailing edge: stacked shrinking
    // rows (the classic 7x4 stepped arrow).
    const float right = snap(box.x + box.width - box.border_right - box.padding_right - 3.0f);
    const float top = snap(box.y + (box.height - 4.0f) / 2.0f);
    for (int row = 0; row < 4; ++row) {
      const float w = 7.0f - 2.0f * static_cast<float>(row);
      list.FillRect(right - w, top + static_cast<float>(row), w, 1, ink);
    }
    break;
  }
  default:
    break;
  }
}

// Disclosure triangle of a <details> summary (WHATWG HTML §4.11.1).  The
// marker gutter was reserved by layout (summary padding-left); the triangle is
// built from one axis-aligned fill per row so it rasterizes deterministically,
// independent of fonts and platforms -- the browser UA's ::marker glyph is not
// used.  Collapsed points right (4x7), open points down (7x4); both are
// pixel-snapped and centred on the summary's first line.
void PaintDetailsMarker(const layout::LayoutBox& box, DisplayList& list)
{
  const css::Color ink = box.style.color.value_or(css::Color{0, 0, 0, 255});
  const auto snap = [](float v) { return std::round(v); };
  const float line_height = !box.lines.empty() ? box.lines.front().height : box.style.line_height;
  const float gap = 16.0f; // keep in sync with kDisclosureMarkerGap in layout.cpp
  const float center_x = box.content_x() - gap / 2.0f;
  const int rows = box.details_open ? 4 : 7;
  const float width = static_cast<float>(box.details_open ? 7 : 4);
  const float left = snap(center_x - width / 2.0f);
  const float top = snap(box.content_y() + (line_height - static_cast<float>(rows)) / 2.0f);
  for (int row = 0; row < rows; ++row) {
    // Collapsed: 1,2,3,4,3,2,1 wide.  Open: 7,5,3,1 wide.
    const float run = box.details_open ? 7.0f - 2.0f * static_cast<float>(row)
                                       : static_cast<float>(row < 4 ? row + 1 : 7 - row);
    const float run_left = box.details_open ? left + static_cast<float>(row) : left;
    list.FillRect(run_left, top + static_cast<float>(row), run, 1, ink);
  }
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

// Out-of-flow painting model (simplified CSS 2.1 Appendix E), see painter.h.
//
// True for a box taken out of normal flow (relative stays in flow and paints
// at its tree position).
bool IsPositioned(const layout::LayoutBox& box)
{
  return box.style.position == style::Position::kAbsolute ||
         box.style.position == style::Position::kFixed;
}

// True when |box| establishes a stacking context (CSS 2.1 §9.9): `opacity`
// below 1 (CSS Color 4 §3.2), or a positioned box with an explicit z-index.
// A z-index:auto positioned box establishes NONE — its positioned descendants
// roll up into the enclosing context's tiers, which is what keeps a deep
// dropdown sortable against the whole page.
bool CreatesStackingContext(const layout::LayoutBox& box)
{
  if (box.style.opacity < 1.0f) {
    return true;
  }
  return IsPositioned(box) && box.style.z_index.has_value();
}

} // namespace

DisplayList Painter::Paint() const
{
  DisplayList list;
  if (root_ == nullptr) {
    return list;
  }
  PaintContext(*root_, list);
  return list;
}

void Painter::PaintContext(const layout::LayoutBox& box, DisplayList& list) const
{
  // Group opacity (CSS Color 4 §3.2): everything this context paints --
  // including its positioned units -- is composited as ONE unit.  A
  // descendant's own opacity nests a smaller group inside.
  const bool grouped = box.style.opacity < 1.0f;
  if (grouped) {
    list.PushGroup(box.style.opacity);
  }

  std::vector<const layout::LayoutBox*> neg;
  std::vector<const layout::LayoutBox*> zero;
  std::vector<const layout::LayoutBox*> pos;
  CollectUnits(box, neg, zero, pos);

  // E1: the context root's own decoration paints first (below even its
  // negative z-index children).  visibility: hidden suppresses it but keeps
  // the layout space; descendants that declare visibility: visible again
  // still paint (CSS 2.2 §11.2).
  if (box.style.visibility != style::Visibility::kHidden) {
    PaintBoxSelf(box, list);
  }

  // E2: negative z-index child contexts, ascending (stable).
  std::stable_sort(
      neg.begin(), neg.end(), [](const layout::LayoutBox* a, const layout::LayoutBox* b) {
        return a->style.z_index.value_or(0) < b->style.z_index.value_or(0);
      });
  for (const layout::LayoutBox* unit : neg) {
    PaintUnit(*unit, list);
  }

  // E3-E5: the in-flow content of this context.
  PaintInFlowContent(box, list);

  // E6: z-index 0/auto units in tree order (positioned content sits above
  // all in-flow content of its context — a dropdown must not be covered by
  // the page content below its header); E7: positive z-index, ascending.
  for (const layout::LayoutBox* unit : zero) {
    PaintUnit(*unit, list);
  }
  std::stable_sort(
      pos.begin(), pos.end(), [](const layout::LayoutBox* a, const layout::LayoutBox* b) {
        return a->style.z_index.value_or(0) < b->style.z_index.value_or(0);
      });
  for (const layout::LayoutBox* unit : pos) {
    PaintUnit(*unit, list);
  }

  if (grouped) {
    list.PopGroup();
  }
}

void Painter::PaintUnit(const layout::LayoutBox& box, DisplayList& list) const
{
  // A fixed subtree is viewport-pinned: the rasterizer suspends the scroll
  // shift between these markers (layout placed the box at its scroll-0
  // document position).
  const bool fixed = box.style.position == style::Position::kFixed;
  if (fixed) {
    list.PushFixed();
  }
  if (CreatesStackingContext(box)) {
    PaintContext(box, list);
  } else {
    // z-index:auto positioned box: no context of its own.  Its in-flow
    // content paints here; its positioned descendants were rolled into the
    // enclosing tiers by CollectUnits.
    if (box.style.visibility != style::Visibility::kHidden) {
      PaintBoxSelf(box, list);
    }
    PaintInFlowContent(box, list);
  }
  if (fixed) {
    list.PopFixed();
  }
}

void Painter::PaintInFlowNode(const layout::LayoutBox& box, DisplayList& list) const
{
  if (CreatesStackingContext(box)) {
    // An in-flow context root (opacity < 1) paints its context at its tree
    // position inside the in-flow content.
    PaintContext(box, list);
    return;
  }
  if (box.style.visibility != style::Visibility::kHidden) {
    PaintBoxSelf(box, list);
  }
  PaintInFlowContent(box, list);
}

void Painter::PaintInFlowContent(const layout::LayoutBox& box, DisplayList& list) const
{
  // overflow: hidden/auto/scroll clips the box's content (and its
  // descendants) to the padding box.  The box's own background/border paint
  // above the clip boundary and are NOT clipped.
  const bool clips = box.style.overflow != style::Overflow::kVisible;
  if (clips) {
    list.PushClip(box.content_x(), box.content_y(), box.content_width(), box.content_height());
  }

  // Block children (back to front).  Positioned children are painted by the
  // enclosing stacking context's unit pass instead.
  for (const auto& child : box.children) {
    if (!IsPositioned(*child)) {
      PaintInFlowNode(*child, list);
    }
  }

  // Floats paint above in-flow block children but below inline content
  // (CSS2.1 Appendix E), so a float's background/text is not covered by a
  // later block-level sibling.
  for (const auto& f : box.floats) {
    if (!IsPositioned(*f)) {
      PaintInFlowNode(*f, list);
    }
  }

  // Atomic inline boxes within lines.
  for (const layout::Line& line : box.lines) {
    for (const layout::InlineBox& inline_box : line.boxes) {
      if (inline_box.block_box != nullptr) {
        // An inline-block: paint its inner block layout (background, border,
        // content and children).  It carries the element's own style.
        PaintInFlowNode(*inline_box.block_box, list);
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
  // block itself duplicate the block's own background, which PaintBoxSelf
  // already filled, so they are skipped.
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

void Painter::CollectUnits(const layout::LayoutBox& box,
                           std::vector<const layout::LayoutBox*>& neg,
                           std::vector<const layout::LayoutBox*>& zero,
                           std::vector<const layout::LayoutBox*>& pos) const
{
  const auto classify = [&neg, &zero, &pos](const layout::LayoutBox* unit) {
    const int z = unit->style.z_index.value_or(0);
    if (z < 0) {
      neg.push_back(unit);
    } else if (z > 0) {
      pos.push_back(unit);
    } else {
      zero.push_back(unit);
    }
  };

  // Walks the box tree in tree order.  A positioned box that establishes its
  // own stacking context is a unit here and its descendants are collected
  // when it paints; a z-index:auto positioned box is a unit but its own
  // positioned descendants roll UP into this context (spec behaviour).
  // In-flow context roots (opacity < 1) collect their own units.
  std::function<void(const layout::LayoutBox&)> visit = [&](const layout::LayoutBox& node) {
    const auto visit_subtree = [&](const layout::LayoutBox& child) {
      if (IsPositioned(child)) {
        classify(&child);
        if (!CreatesStackingContext(child)) {
          visit(child);
        }
      } else if (!CreatesStackingContext(child)) {
        visit(child);
      }
    };
    for (const auto& child : node.positioned_children) {
      visit_subtree(*child);
    }
    for (const auto& child : node.children) {
      visit_subtree(*child);
    }
    for (const auto& f : node.floats) {
      visit_subtree(*f);
    }
    for (const layout::Line& line : node.lines) {
      for (const layout::InlineBox& inline_box : line.boxes) {
        if (inline_box.block_box != nullptr) {
          visit_subtree(*inline_box.block_box);
        }
      }
    }
  };
  visit(box);
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

  // Native widget innards (checkbox tick, radio dot, drop-down arrow) sit on
  // top of the chrome and under the content.
  PaintWidget(box, list);

  // A <summary> disclosure control draws its triangle in the gutter layout
  // reserved for it.
  if (box.details_summary) {
    PaintDetailsMarker(box, list);
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
