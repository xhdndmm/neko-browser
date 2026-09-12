#pragma once

#include "neko/base/status.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace neko::compositor {

// A GPU context: the engine's thin wrapper over a platform graphics API.
//
// STATUS: PARTIALLY IMPLEMENTED.  The interface, the handle types and the
// no-op fallback context are real; the per-platform backends
// (GL 3.3 / Vulkan 1.1 / Metal / D3D11) are NOT IMPLEMENTED.  Everything the
// compositor needs is expressed here so a backend can be dropped in without
// changing GpuCompositor.
//
// Design notes
// ------------
//  * The context owns device resources; one context per compositor.
//  * Textures are the unit of layer storage (one per compositor layer).
//  * Drawing is expressed as a small, fixed set of operations the compositor
//    needs (draw a textured quad, clear, present) rather than exposing the
//    whole graphics API.  That keeps call sites backend-agnostic and testable
//    against a recording fake.
//  * Threading: a context is confined to the thread that created it.

enum class GpuPrimitive
{
  TexturedQuad, // the only primitive the compositor uses today
};

// Describes one draw call.  Texture coordinates are normalized 0..1.
struct GpuDrawCall
{
  GpuPrimitive primitive = GpuPrimitive::TexturedQuad;
  std::uintptr_t texture = 0; // opaque GpuContext texture handle
  float x = 0.0f;             // destination rectangle in output pixels
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  float opacity = 1.0f;
};

// A minimal GPU device abstraction.  Backends implement this; the compositor
// only ever sees these methods.
class GpuContext
{
public:
  virtual ~GpuContext() = default;

  GpuContext(const GpuContext&) = delete;
  GpuContext& operator=(const GpuContext&) = delete;

  // Human-readable identity, for diagnostics and the DevTools GPU panel.
  virtual std::string renderer() const = 0;
  virtual std::string vendor() const = 0;
  virtual std::string version() const = 0;
  virtual bool hardware_accelerated() const = 0;
  virtual int max_texture_size() const = 0;

  // Creates (or re-creates) the presentation surface of the given size.
  virtual base::Status ResizeSurface(int width, int height) = 0;

  // Creates a texture holding |width| x |height| RGBA8888 pixels.  |handle|
  // receives an opaque identifier used by Upload/Draw/Present.  Returns a
  // NOT IMPLEMENTED status without a backend.
  virtual base::Status CreateTexture(int width, int height, std::uintptr_t* handle) = 0;

  // Uploads RGBA8888 pixels into the sub-rectangle of |handle|.  |stride| is
  // the source row pitch in bytes.
  virtual base::Status UploadTexture(std::uintptr_t handle,
                                     const std::uint8_t* pixels,
                                     int stride,
                                     int x,
                                     int y,
                                     int w,
                                     int h) = 0;

  virtual void DestroyTexture(std::uintptr_t handle) = 0;

  // Clears the current frame to |rgba| (0xRRGGBBAA).
  virtual void BeginFrame(std::uint32_t rgba) = 0;

  virtual void Draw(const GpuDrawCall& call) = 0;

  // Presents the finished frame.
  virtual base::Status EndFrame() = 0;

  // Copies the presented frame back into |out_rgba8888| (size =
  // width*height*4).  Used by the headless screenshot path and by tests that
  // compare the GPU output against the software reference.
  virtual base::Status Readback(std::vector<std::uint8_t>* out_rgba8888) = 0;

protected:
  GpuContext() = default;
};

// A context that satisfies the interface without a device: every operation
// either succeeds trivially (ResizeSurface/BeginFrame/Draw) or returns a NOT
// IMPLEMENTED error (textures, present, readback).  The compositor uses it in
// pass-through mode, which keeps the software surface path exercised even on
// machines with no GPU.
class NullGpuContext final : public GpuContext
{
public:
  std::string renderer() const override
  {
    return "none";
  }
  std::string vendor() const override
  {
    return "none";
  }
  std::string version() const override
  {
    return "0";
  }
  bool hardware_accelerated() const override
  {
    return false;
  }
  int max_texture_size() const override
  {
    return 0;
  }

  base::Status ResizeSurface(int width, int height) override;
  base::Status CreateTexture(int width, int height, std::uintptr_t* handle) override;
  base::Status UploadTexture(std::uintptr_t handle,
                             const std::uint8_t* pixels,
                             int stride,
                             int x,
                             int y,
                             int w,
                             int h) override;
  void DestroyTexture(std::uintptr_t handle) override;
  void BeginFrame(std::uint32_t rgba) override;
  void Draw(const GpuDrawCall& call) override;
  base::Status EndFrame() override;
  base::Status Readback(std::vector<std::uint8_t>* out_rgba8888) override;

  int surface_width() const
  {
    return width_;
  }
  int surface_height() const
  {
    return height_;
  }

private:
  int width_ = 0;
  int height_ = 0;
};

// A recording context for tests: implements the interface by recording draw
// calls and texture contents, so the compositor's GPU path can be verified
// without a device.  This is the reference used by the GPU compositor tests.
class RecordingGpuContext final : public GpuContext
{
public:
  struct RecordedTexture
  {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels; // RGBA8888
  };

  std::string renderer() const override
  {
    return "recording";
  }
  std::string vendor() const override
  {
    return "test";
  }
  std::string version() const override
  {
    return "1";
  }
  bool hardware_accelerated() const override
  {
    return false;
  }
  int max_texture_size() const override
  {
    return 8192;
  }

  base::Status ResizeSurface(int width, int height) override;
  base::Status CreateTexture(int width, int height, std::uintptr_t* handle) override;
  base::Status UploadTexture(std::uintptr_t handle,
                             const std::uint8_t* pixels,
                             int stride,
                             int x,
                             int y,
                             int w,
                             int h) override;
  void DestroyTexture(std::uintptr_t handle) override;
  void BeginFrame(std::uint32_t rgba) override;
  void Draw(const GpuDrawCall& call) override;
  base::Status EndFrame() override;
  base::Status Readback(std::vector<std::uint8_t>* out_rgba8888) override;

  // Test-facing accessors.
  const std::vector<GpuDrawCall>& draws() const
  {
    return draws_;
  }
  const std::vector<RecordedTexture>& textures() const
  {
    return textures_;
  }
  int clear_color() const
  {
    return static_cast<int>(clear_rgba_);
  }
  int frames_presented() const
  {
    return frames_presented_;
  }
  void Reset();

private:
  struct Slot
  {
    bool used = false;
    RecordedTexture texture;
  };

  int width_ = 0;
  int height_ = 0;
  std::vector<Slot> slots_;
  std::vector<GpuDrawCall> draws_;
  std::vector<RecordedTexture> textures_; // snapshots created at EndFrame
  std::uint32_t clear_rgba_ = 0;
  int frames_presented_ = 0;
};

} // namespace neko::compositor
