#include "neko/compositor/compositor.h"

#include <algorithm>

namespace neko::compositor {

SoftwareCompositor::SoftwareCompositor(int width, int height)
{
  Resize(width, height);
}

void SoftwareCompositor::Resize(int width, int height)
{
  output_.Resize(width, height);
}

Surface& SoftwareCompositor::Output()
{
  return output_;
}

const Surface& SoftwareCompositor::Output() const
{
  return output_;
}

Layer& SoftwareCompositor::EnsureLayer(std::size_t index)
{
  if (layers_.size() <= index) {
    layers_.resize(index + 1);
  }
  return layers_[index];
}

Surface& SoftwareCompositor::LayerSurface(std::size_t index)
{
  return EnsureLayer(index).surface;
}

void SoftwareCompositor::SetLayerPlacement(std::size_t index, int x, int y)
{
  Layer& layer = EnsureLayer(index);
  layer.x = x;
  layer.y = y;
}

void SoftwareCompositor::SetLayerOpacity(std::size_t index, float opacity)
{
  EnsureLayer(index).opacity = std::clamp(opacity, 0.0f, 1.0f);
}

void SoftwareCompositor::SetLayerVisible(std::size_t index, bool visible)
{
  EnsureLayer(index).visible = visible;
}

void SoftwareCompositor::Composite(css::Color background)
{
  output_.Clear(background);
  if (layers_.empty()) {
    return;
  }
  const Layer& page = layers_[0];
  if (page.visible) {
    output_.CopyFrom(page.surface, page.x, page.y);
  }
  for (std::size_t i = 1; i < layers_.size(); ++i) {
    const Layer& layer = layers_[i];
    if (!layer.visible) {
      continue;
    }
    output_.BlendOver(layer.surface, layer.x, layer.y, layer.opacity);
  }
}

void SoftwareCompositor::CompositeRect(int x, int y, int w, int h)
{
  if (w <= 0 || h <= 0 || layers_.empty()) {
    return;
  }
  const Layer& page = layers_[0];
  if (page.visible) {
    // Restore the backdrop from layer 0 for the dirty region.
    output_.CopyRect(page.surface, x - page.x, y - page.y, w, h, x, y);
  }
  for (std::size_t i = 1; i < layers_.size(); ++i) {
    const Layer& layer = layers_[i];
    if (!layer.visible) {
      continue;
    }
    output_.BlendOver(layer.surface, layer.x, layer.y, layer.opacity, x, y, w, h);
  }
}

void SoftwareCompositor::ScrollOutput(int delta, int* band_y0, int* band_y1)
{
  *band_y0 = 0;
  *band_y1 = 0;
  output_.ShiftRows(delta);
  const int height = output_.height();
  if (delta > 0) {
    *band_y0 = 0;
    *band_y1 = std::min(delta, height);
  } else if (delta < 0) {
    *band_y0 = std::max(0, height + delta);
    *band_y1 = height;
  }
}

} // namespace neko::compositor
