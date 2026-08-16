#pragma once

#include "neko/compositor/surface.h"
#include "neko/css/color.h"

#include <cstddef>
#include <vector>

namespace neko::compositor {

// A compositing layer: an owned surface plus its placement, opacity and
// visibility.  Layer 0 is the page backdrop (copied, not blended); overlays
// (index > 0) are alpha-blended over everything below them in index order.
struct Layer
{
  Surface surface;
  int x = 0;
  int y = 0;
  float opacity = 1.0f; // 0..1, scales the surface's alpha on blend
  bool visible = true;
};

// Abstraction over the presentation of painted content (ADR 0015).  The
// compositor owns the final output surface and an ordered list of layers;
// producing a frame composites the layers onto the output.  The software
// implementation composites RGBA surfaces on the CPU; a GPU implementation
// can provide the same interface later without touching the UI.
//
// Layer 0 is the page's rasterized content (opaque copy).  Overlays shift
// with the content on ScrollOutput, so they stay glued to the page.
//
// Threading: confined to one thread (the GUI thread in practice); members are
// not synchronized.
class Compositor
{
public:
  virtual ~Compositor() = default;

  virtual void Resize(int width, int height) = 0;

  // The surface the UI presents (blitted to the window).
  virtual Surface& Output() = 0;
  virtual const Surface& Output() const = 0;

  // Mutable layer surfaces, created on demand up to |index|.
  virtual Surface& LayerSurface(std::size_t index) = 0;
  virtual void SetLayerPlacement(std::size_t index, int x, int y) = 0;
  virtual void SetLayerOpacity(std::size_t index, float opacity) = 0;
  virtual void SetLayerVisible(std::size_t index, bool visible) = 0;

  // Full composite: clears the output to |background|, copies layer 0, then
  // blends every visible overlay in order.
  virtual void Composite(css::Color background) = 0;

  // Re-composites one dirty axis-aligned region (clipped to the output):
  // restores layer 0's pixels there and re-blends every visible overlay
  // intersecting it.  Cheaper than Composite() for small invalidation rects
  // (e.g. a blinking caret).
  virtual void CompositeRect(int x, int y, int w, int h) = 0;

  // Scrolls the output by |delta| rows (positive = content moves down) and
  // reports the exposed band [*band_y0, *band_y1) that the caller must refill
  // from layer 0 (both 0 when nothing was exposed).  Overlay pixels shift
  // with the content, so overlays stay glued to the page.
  virtual void ScrollOutput(int delta, int* band_y0, int* band_y1) = 0;
};

// CPU implementation: RGBA8888 surfaces blended with integer fixed-point math
// (mirrors paint::Rasterizer's blend rounding).
class SoftwareCompositor final : public Compositor
{
public:
  SoftwareCompositor() = default;
  explicit SoftwareCompositor(int width, int height);

  void Resize(int width, int height) override;
  Surface& Output() override;
  const Surface& Output() const override;
  Surface& LayerSurface(std::size_t index) override;
  void SetLayerPlacement(std::size_t index, int x, int y) override;
  void SetLayerOpacity(std::size_t index, float opacity) override;
  void SetLayerVisible(std::size_t index, bool visible) override;
  void Composite(css::Color background) override;
  void CompositeRect(int x, int y, int w, int h) override;
  void ScrollOutput(int delta, int* band_y0, int* band_y1) override;

private:
  Layer& EnsureLayer(std::size_t index);

  Surface output_;
  std::vector<Layer> layers_;
};

} // namespace neko::compositor
