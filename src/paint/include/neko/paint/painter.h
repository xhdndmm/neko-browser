#pragma once

#include "neko/layout/layout_tree.h"
#include "neko/paint/display_list.h"

namespace neko::paint {

// Converts a laid-out page into a display list.
//
// Painting follows a simplified CSS 2.1 Appendix E, organised as stacking
// contexts (CSS 2.1 §9.9).  Within one context, paint order is:
//   1. the context root's own decoration (background, border, replaced
//      content),
//   2. negative z-index child contexts (ascending),
//   3. the in-flow content: block children in tree order (each box's own
//      decoration before its children), floats, then inline content,
//   4. z-index 0/auto positioned units in tree order,
//   5. positive z-index child contexts (ascending).
//
// A stacking context is established by the root, by `opacity < 1` (CSS Color
// 4 §3.2 — that is also what group opacity wraps), or by a positioned box
// with an explicit z-index.  A z-index:auto positioned box establishes none:
// its in-flow content paints as one unit while its positioned descendants
// roll up into the enclosing context's tiers (spec behaviour).  The box tree
// walk that finds units treats the containing-block attachment
// (LayoutBox::positioned_children) as the ancestry.
//
// A relative-positioned box with an explicit z-index stays in flow for layout
// but joins the parent's z-order and establishes a stacking context (CSS 2.1
// §9.9).  Known approximation: in-flow content paints per box (decoration and
// content interleaved) rather than in the global E3/E4/E5 phases.
class Painter
{
public:
  explicit Painter(const layout::LayoutBox* root) : root_(root) {}

  DisplayList Paint() const;

private:
  // Paints one stacking context: own decoration, negative units, in-flow
  // content, then z-auto and positive units.  Wraps the whole thing in a
  // group when the root has opacity < 1.
  void PaintContext(const layout::LayoutBox& box, DisplayList& list) const;

  // Paints one positioned unit of a context: a full context when it
  // establishes one, otherwise its decoration + in-flow content (its own
  // positioned descendants are already collected in the enclosing tiers).
  void PaintUnit(const layout::LayoutBox& box, DisplayList& list) const;

  // Paints an in-flow subtree node: delegates to PaintContext when the node
  // establishes one (an `opacity < 1` box), else decoration + in-flow content.
  void PaintInFlowNode(const layout::LayoutBox& box, DisplayList& list) const;

  // In-flow content of |box| (block children, floats, inline boxes, inline
  // backgrounds, text runs) under the overflow clip — everything but |box|'s
  // own decoration and its positioned units.
  void PaintInFlowContent(const layout::LayoutBox& box, DisplayList& list) const;

  // Paints the box's own decorations and content (background, border,
  // replaced image); the caller gates this on visibility and handles the
  // clipping + child recursion.
  void PaintBoxSelf(const layout::LayoutBox& box, DisplayList& list) const;

  // Collects the positioned units belonging to |box|'s stacking context into
  // the negative / zero / positive z-index tiers, in tree order within each.
  void CollectUnits(const layout::LayoutBox& box,
                    std::vector<const layout::LayoutBox*>& neg,
                    std::vector<const layout::LayoutBox*>& zero,
                    std::vector<const layout::LayoutBox*>& pos) const;

  const layout::LayoutBox* root_;
};

} // namespace neko::paint
