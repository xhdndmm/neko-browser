#pragma once

#include "neko/layout/layout_tree.h"
#include "neko/paint/display_list.h"

namespace neko::paint {

// Converts a laid-out page into a display list.
//
// Painting order: box background, border, inline text runs, then block
// children (back to front).  Only solid colors are emitted; images and other
// content types are not supported yet.
class Painter
{
public:
  explicit Painter(const layout::LayoutBox* root) : root_(root) {}

  DisplayList Paint() const;

private:
  void PaintBox(const layout::LayoutBox& box, DisplayList& list) const;

  const layout::LayoutBox* root_;
};

} // namespace neko::paint
