// GPU compositor front end (ADR 0017).
//
// STATUS: PARTIALLY IMPLEMENTED.
//
// What is real here:
//   * The Compositor interface implementation (layer bookkeeping, dirty
//     rectangles, scroll-band reporting) is complete and tested.
//   * The CPU surface path: every layer keeps its CPU pixels, so the presented
//     output is always correct regardless of whether a device is present.
//   * The texture ledger: dirty layers are uploaded to device textures and one
//     textured quad is issued per visible layer.  RecordingGpuContext verifies
//     the ledger without a device.
//   * The capability probe and the software-fallback factory.
//
// What is real since the Linux backend landed:
//   * ProbeGpuCapabilities() creates a real EGL/OpenGL context (when the
//     platform can provide one) and reports its identity and limits.
//   * Create() builds a GpuCompositor around that context; the device path
//     uploads dirty layers and composes them with the same fixed-point math
//     the software compositor uses (verified byte-for-byte on the GPU).
//
// What is NOT IMPLEMENTED:
//   * Non-Linux backends (Vulkan/Metal/Direct3D) and window-system swapchain
//     presentation: the compositor's presentable output is still the CPU
//     surface (ADR 0017); the device output is read back through
//     GpuContext::Readback for verification and headless screenshots.

#include "neko/compositor/gpu_compositor.h"

#include "neko/base/logging.h"

#include <algorithm>

namespace neko::compositor {
namespace {

void LogDeviceNotImplementedOnce(bool* warned)
{
  if (*warned) {
    return;
  }
  *warned = true;
  NEKO_LOG_WARNING("GPU device operation failed; compositing on the CPU");
}

} // namespace

const char* GpuBackendName(GpuBackend backend)
{
  switch (backend) {
  case GpuBackend::OpenGL:
    return "OpenGL";
  case GpuBackend::Vulkan:
    return "Vulkan";
  case GpuBackend::Metal:
    return "Metal";
  case GpuBackend::Direct3D:
    return "Direct3D";
  case GpuBackend::None:
    break;
  }
  return "None";
}

GpuCapabilities ProbeGpuCapabilities()
{
  // Create and immediately destroy a real context: probing by asking the
  // device avoids claiming capabilities the compositor could not actually use.
  std::unique_ptr<GpuContext> context = CreateBestGpuContext();
  if (!context) {
    return GpuCapabilities{};
  }

  GpuCapabilities caps;
  caps.available = true;
  caps.backend = context->backend();
  caps.renderer = context->renderer();
  caps.vendor = context->vendor();
  caps.version = context->version();
  caps.max_texture_size = context->max_texture_size();
  // OpenGL 3.3 core (the only backend today) guarantees non-power-of-two
  // textures; future GLES2-level backends must query this honestly.
  caps.supports_npot_textures = true;
  caps.hardware_accelerated = context->hardware_accelerated();
  return caps;
}

// ---------------------------------------------------------------------------
// GpuCompositor
// ---------------------------------------------------------------------------

GpuCompositor::GpuCompositor(int width, int height, std::unique_ptr<GpuContext> context)
    : width_(std::max(0, width)), height_(std::max(0, height)), context_(std::move(context))
{
  output_.Resize(width_, height_);
}

GpuCompositor::~GpuCompositor() = default;

std::unique_ptr<Compositor> GpuCompositor::Create(int width, int height, bool force_software)
{
  if (!force_software) {
    std::unique_ptr<GpuContext> context = CreateBestGpuContext();
    if (context) {
      // The device must accept the requested surface before we commit to the
      // GPU path; a context that cannot size its surface is not usable.
      if (context->ResizeSurface(width, height)) {
        return std::unique_ptr<Compositor>(new GpuCompositor(width, height, std::move(context)));
      }
      NEKO_LOG_WARNING("GPU context could not resize its surface; using the software compositor");
    }
  }
  // No device: the software compositor is the correct implementation, not a
  // degraded stand-in.
  return std::make_unique<SoftwareCompositor>(width, height);
}

std::unique_ptr<GpuCompositor>
GpuCompositor::CreateWithContext(int width, int height, std::unique_ptr<GpuContext> context)
{
  // The compositor's GPU path requires a sized device surface before the first
  // frame; the CPU mirror remains correct even if this fails, so the failure
  // is reported through the usual device-error path at present time.
  if (context) {
    (void)context->ResizeSurface(width, height);
  }
  return std::unique_ptr<GpuCompositor>(new GpuCompositor(width, height, std::move(context)));
}

GpuCompositor::LayerState& GpuCompositor::EnsureLayerState(std::size_t index)
{
  if (layer_states_.size() <= index) {
    layer_states_.resize(index + 1);
  }
  return layer_states_[index];
}

void GpuCompositor::Resize(int width, int height)
{
  width_ = std::max(0, width);
  height_ = std::max(0, height);
  output_.Resize(width_, height_);
  if (context_) {
    // The software path owns presentation; a failed surface resize only
    // affects the device path, which is not implemented yet.
    (void)context_->ResizeSurface(width_, height_);
  }
  for (LayerState& state : layer_states_) {
    state.texture_dirty = true;
  }
}

Surface& GpuCompositor::Output()
{
  return output_;
}

const Surface& GpuCompositor::Output() const
{
  return output_;
}

Surface& GpuCompositor::LayerSurface(std::size_t index)
{
  if (layers_.size() <= index) {
    layers_.resize(index + 1);
  }
  EnsureLayerState(index).texture_dirty = true;
  return layers_[index];
}

void GpuCompositor::SetLayerPlacement(std::size_t index, int x, int y)
{
  LayerState& state = EnsureLayerState(index);
  state.x = x;
  state.y = y;
}

void GpuCompositor::SetLayerOpacity(std::size_t index, float opacity)
{
  EnsureLayerState(index).opacity = std::clamp(opacity, 0.0f, 1.0f);
}

void GpuCompositor::SetLayerVisible(std::size_t index, bool visible)
{
  EnsureLayerState(index).visible = visible;
}

void GpuCompositor::UploadLayer(std::size_t index)
{
  if (!context_ || index >= layers_.size() || index >= layer_states_.size()) {
    return;
  }
  Surface& layer = layers_[index];
  if (layer.width() <= 0 || layer.height() <= 0) {
    return;
  }
  LayerState& state = layer_states_[index];

  std::uintptr_t handle = 0;
  if (state.texture_handle == 0) {
    auto created = context_->CreateTexture(layer.width(), layer.height(), &handle);
    if (!created) {
      LogDeviceNotImplementedOnce(&warned_not_implemented_);
      return;
    }
    state.texture_handle = handle;
  }

  auto uploaded = context_->UploadTexture(state.texture_handle,
                                          layer.pixels().data(),
                                          layer.width() * 4,
                                          0,
                                          0,
                                          layer.width(),
                                          layer.height());
  if (!uploaded) {
    LogDeviceNotImplementedOnce(&warned_not_implemented_);
    return;
  }
  state.texture_dirty = false;
  ++texture_uploads_;
}

void GpuCompositor::DrawLayers()
{
  if (!context_) {
    return;
  }
  for (std::size_t i = 0; i < layers_.size(); ++i) {
    LayerState& state = layer_states_[i];
    if (!state.visible || state.texture_handle == 0) {
      continue;
    }
    GpuDrawCall call;
    call.texture = state.texture_handle;
    call.x = static_cast<float>(state.x);
    call.y = static_cast<float>(state.y);
    call.width = static_cast<float>(layers_[i].width());
    call.height = static_cast<float>(layers_[i].height());
    call.opacity = i == 0 ? 1.0f : state.opacity;
    // Layer 0 is a raw copy (Surface::CopyFrom), overlays blend
    // (Surface::BlendOver); the device must not second-guess that difference.
    call.blend = i == 0 ? GpuBlendMode::kCopy : GpuBlendMode::kOver;
    context_->Draw(call);
    ++draw_calls_;
  }
}

void GpuCompositor::Composite(css::Color background)
{
  // CPU reference path: keep the presented surface correct in all cases.
  output_.Clear(background);
  if (!layers_.empty()) {
    const LayerState& page_state = layer_states_[0];
    if (page_state.visible) {
      output_.CopyFrom(layers_[0], page_state.x, page_state.y);
    }
    for (std::size_t i = 1; i < layers_.size(); ++i) {
      const LayerState& state = layer_states_[i];
      if (!state.visible) {
        continue;
      }
      output_.BlendOver(layers_[i], state.x, state.y, state.opacity);
    }
  }

  if (context_) {
    context_->BeginFrame((static_cast<std::uint32_t>(background.r) << 24) |
                         (static_cast<std::uint32_t>(background.g) << 16) |
                         (static_cast<std::uint32_t>(background.b) << 8) |
                         static_cast<std::uint32_t>(background.a));
    for (std::size_t i = 0; i < layer_states_.size(); ++i) {
      if (layer_states_[i].texture_dirty) {
        UploadLayer(i);
      }
    }
    DrawLayers();
    auto presented = context_->EndFrame();
    if (!presented) {
      LogDeviceNotImplementedOnce(&warned_not_implemented_);
    }
  }
}

void GpuCompositor::CompositeRect(int x, int y, int w, int h)
{
  if (w <= 0 || h <= 0 || layers_.empty()) {
    return;
  }
  const LayerState& page_state = layer_states_[0];
  if (page_state.visible) {
    output_.CopyRect(layers_[0], x - page_state.x, y - page_state.y, w, h, x, y);
  }
  for (std::size_t i = 1; i < layers_.size(); ++i) {
    const LayerState& state = layer_states_[i];
    if (!state.visible) {
      continue;
    }
    output_.BlendOver(layers_[i], state.x, state.y, state.opacity, x, y, w, h);
  }

  if (context_) {
    // A real backend would re-upload only the intersecting sub-rectangles and
    // re-issue the affected draw calls here.  Without a backend, mark the
    // layers dirty so the next full Composite stays coherent.
    for (LayerState& state : layer_states_) {
      state.texture_dirty = true;
    }
  }
}

void GpuCompositor::ScrollOutput(int delta, int* band_y0, int* band_y1)
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
  // Overlays shift with the content, exactly like the software compositor.
  if (context_) {
    for (LayerState& state : layer_states_) {
      state.texture_dirty = true;
    }
  }
}

} // namespace neko::compositor
