#pragma once

#include "neko/compositor/compositor.h"
#include "neko/compositor/gpu_context.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace neko::compositor {

// GPU-accelerated compositor backend (ADR 0017).
//
// STATUS: PARTIALLY IMPLEMENTED — the interface, layer bookkeeping, dirty-rect
// handling, texture ledger and the software fallback are real and tested; an
// actual platform GPU backend (GL/Vulkan/Metal/D3D11) is NOT IMPLEMENTED.
// On machines without a usable GPU the factory returns a SoftwareCompositor
// and callers see identical output.
//
// Intended GPU design (mirrors Chromium's `cc` in miniature): one texture per
// compositing layer, dirty sub-rectangle uploads, one textured quad per layer
// with alpha blending into the swapchain image.  The compositor talks to the
// device only through GpuContext, so a backend can be added without touching
// this class; RecordingGpuContext lets the upload/draw ledger be tested today.

// Whether the engine can create a working GPU context on this machine.
enum class GpuBackend
{
  None,     // no GPU API available
  OpenGL,   // desktop OpenGL 3.3+ / OpenGL ES 3.0+
  Vulkan,   // Vulkan 1.1+
  Metal,    // Apple Metal (macOS)
  Direct3D, // Direct3D 11 (Windows)
};

const char* GpuBackendName(GpuBackend backend);

// Probed capabilities.  |available| false means the GPU path is unusable and
// the compositor must fall back to software.
struct GpuCapabilities
{
  bool available = false;
  GpuBackend backend = GpuBackend::None;
  std::string renderer; // e.g. "llvmpipe" / "NVIDIA GeForce ..."
  std::string vendor;
  std::string version;
  int max_texture_size = 0;
  bool supports_npot_textures = false;
  bool hardware_accelerated = false; // false for software rasterizers (llvmpipe)
};

// Probes the platform for a usable GPU context.  Never throws; a machine
// without a GPU (or without the required driver) yields {available = false}.
// The probe is cheap and safe to call at startup.
//
// Implementation status: returns {available = false} on all platforms today.
GpuCapabilities ProbeGpuCapabilities();

// GPU compositor front end.  Presents the Compositor interface; every frame it
// keeps the CPU surface path correct, and (when a device is present) uploads
// dirty layer rectangles and issues one draw call per visible layer.
//
// Threading: confined to one thread, like the software compositor.
class GpuCompositor final : public Compositor
{
public:
  // Creates the best available compositor.  With a usable GPU returns a
  // GpuCompositor driven by that device; otherwise returns a
  // SoftwareCompositor (never nullptr).  |force_software| skips the probe,
  // which tests use to keep results deterministic.
  static std::unique_ptr<Compositor> Create(int width, int height, bool force_software = false);

  // Builds a compositor around an explicit context (test seam: pass a
  // RecordingGpuContext to assert the upload/draw ledger).  |context| must not
  // be null.
  static std::unique_ptr<GpuCompositor>
  CreateWithContext(int width, int height, std::unique_ptr<GpuContext> context);

  ~GpuCompositor() override;

  GpuCompositor(const GpuCompositor&) = delete;
  GpuCompositor& operator=(const GpuCompositor&) = delete;

  // Compositor interface.
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

  // True when this instance drives a device context.
  bool using_gpu() const
  {
    return context_ != nullptr;
  }

  // Number of texture uploads and draw calls issued so far (diagnostics and
  // tests).
  long long texture_uploads() const
  {
    return texture_uploads_;
  }
  long long draw_calls() const
  {
    return draw_calls_;
  }

  GpuContext* context()
  {
    return context_.get();
  }

private:
  GpuCompositor(int width, int height, std::unique_ptr<GpuContext> context);

  struct LayerState
  {
    int x = 0;
    int y = 0;
    float opacity = 1.0f;
    bool visible = true;
    bool texture_dirty = true;
    std::size_t texture_handle = 0;
  };

  LayerState& EnsureLayerState(std::size_t index);

  // Uploads the layer's pixels to its GPU texture (full-texture upload; a
  // dirty-rect specialization belongs with the real backend).
  void UploadLayer(std::size_t index);

  // Issues one textured-quad draw per visible layer for the frame.
  void DrawLayers();

  int width_ = 0;
  int height_ = 0;

  // Owned device context.  Null means "no device": the compositor then runs
  // the CPU path only.
  std::unique_ptr<GpuContext> context_;

  // CPU-side mirror of every layer's pixels.  Kept for both paths so the
  // output is always correct and so the GPU result can be compared against
  // the software reference in tests.
  std::vector<Surface> layers_;
  std::vector<LayerState> layer_states_;

  // The surface the UI presents.
  Surface output_;

  long long texture_uploads_ = 0;
  long long draw_calls_ = 0;
  bool warned_not_implemented_ = false;
};

} // namespace neko::compositor
