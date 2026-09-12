// GPU context implementations: the no-device fallback and the recording test
// double.  See gpu_context.h for the porting contract and status.

#include "neko/compositor/gpu_context.h"

#include <algorithm>
#include <cstring>

namespace neko::compositor {
namespace {

// A stable, non-zero handle range for the recording context.  Slot indices are
// 1-based so 0 can mean "invalid" for callers that default-initialize handles.
constexpr std::uintptr_t kHandleBase = 0x9000;

} // namespace

// ---------------------------------------------------------------------------
// NullGpuContext
// ---------------------------------------------------------------------------

base::Status NullGpuContext::ResizeSurface(int width, int height)
{
  width_ = std::max(0, width);
  height_ = std::max(0, height);
  return {};
}

base::Status NullGpuContext::CreateTexture(int /*width*/, int /*height*/, std::uintptr_t* handle)
{
  if (handle != nullptr) {
    *handle = 0;
  }
  return base::Err(base::Error::NotImplemented("GPU texture creation is not implemented"));
}

base::Status NullGpuContext::UploadTexture(std::uintptr_t /*handle*/,
                                           const std::uint8_t* /*pixels*/,
                                           int /*stride*/,
                                           int /*x*/,
                                           int /*y*/,
                                           int /*w*/,
                                           int /*h*/)
{
  return base::Err(base::Error::NotImplemented("GPU texture upload is not implemented"));
}

void NullGpuContext::DestroyTexture(std::uintptr_t /*handle*/) {}

void NullGpuContext::BeginFrame(std::uint32_t /*rgba*/) {}

void NullGpuContext::Draw(const GpuDrawCall& /*call*/) {}

base::Status NullGpuContext::EndFrame()
{
  return base::Err(base::Error::NotImplemented("GPU present is not implemented"));
}

base::Status NullGpuContext::Readback(std::vector<std::uint8_t>* out_rgba8888)
{
  if (out_rgba8888 != nullptr) {
    out_rgba8888->clear();
  }
  return base::Err(base::Error::NotImplemented("GPU readback is not implemented"));
}

// ---------------------------------------------------------------------------
// RecordingGpuContext
// ---------------------------------------------------------------------------

base::Status RecordingGpuContext::ResizeSurface(int width, int height)
{
  width_ = std::max(0, width);
  height_ = std::max(0, height);
  return {};
}

base::Status RecordingGpuContext::CreateTexture(int width, int height, std::uintptr_t* handle)
{
  if (width <= 0 || height <= 0) {
    return base::Err(base::Error::InvalidArgument("texture dimensions must be positive"));
  }
  if (width > max_texture_size() || height > max_texture_size()) {
    return base::Err(base::Error::InvalidArgument("texture exceeds max texture size"));
  }
  // Reuse a free slot when possible so handles stay dense.
  std::size_t index = slots_.size();
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (!slots_[i].used) {
      index = i;
      break;
    }
  }
  if (index == slots_.size()) {
    slots_.emplace_back();
  }
  Slot& slot = slots_[index];
  slot.used = true;
  slot.texture.width = width;
  slot.texture.height = height;
  slot.texture.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4,
                             0);
  if (handle != nullptr) {
    *handle = kHandleBase + index + 1;
  }
  return {};
}

base::Status RecordingGpuContext::UploadTexture(
    std::uintptr_t handle, const std::uint8_t* pixels, int stride, int x, int y, int w, int h)
{
  if (handle < kHandleBase || handle - kHandleBase - 1 >= slots_.size()) {
    return base::Err(base::Error::InvalidArgument("unknown texture handle"));
  }
  Slot& slot = slots_[handle - kHandleBase - 1];
  if (!slot.used) {
    return base::Err(base::Error::InvalidArgument("texture is not alive"));
  }
  if (pixels == nullptr) {
    return base::Err(base::Error::InvalidArgument("pixels must not be null"));
  }

  RecordedTexture& texture = slot.texture;
  const int src_x0 = std::max(0, -x);
  const int src_y0 = std::max(0, -y);
  const int dst_x0 = std::max(0, x);
  const int dst_y0 = std::max(0, y);
  const int copy_w = std::min(w - src_x0, texture.width - dst_x0);
  const int copy_h = std::min(h - src_y0, texture.height - dst_y0);
  if (copy_w <= 0 || copy_h <= 0) {
    return {};
  }

  for (int row = 0; row < copy_h; ++row) {
    const std::size_t src_row = static_cast<std::size_t>(src_y0 + row);
    const std::size_t dst_row = static_cast<std::size_t>(dst_y0 + row);
    const std::uint8_t* src =
        pixels + src_row * static_cast<std::size_t>(stride) + static_cast<std::size_t>(src_x0) * 4;
    std::uint8_t* dst =
        texture.pixels.data() +
        (dst_row * static_cast<std::size_t>(texture.width) + static_cast<std::size_t>(dst_x0)) * 4;
    std::memcpy(dst, src, static_cast<std::size_t>(copy_w) * 4);
  }
  return {};
}

void RecordingGpuContext::DestroyTexture(std::uintptr_t handle)
{
  if (handle < kHandleBase || handle - kHandleBase - 1 >= slots_.size()) {
    return;
  }
  slots_[handle - kHandleBase - 1] = Slot{};
}

void RecordingGpuContext::BeginFrame(std::uint32_t rgba)
{
  draws_.clear();
  clear_rgba_ = rgba;
}

void RecordingGpuContext::Draw(const GpuDrawCall& call)
{
  draws_.push_back(call);
}

base::Status RecordingGpuContext::EndFrame()
{
  textures_.clear();
  for (const Slot& slot : slots_) {
    if (slot.used) {
      textures_.push_back(slot.texture);
    }
  }
  ++frames_presented_;
  return {};
}

base::Status RecordingGpuContext::Readback(std::vector<std::uint8_t>* out_rgba8888)
{
  if (out_rgba8888 == nullptr) {
    return base::Err(base::Error::InvalidArgument("output buffer must not be null"));
  }
  out_rgba8888->assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4, 0);
  return {};
}

void RecordingGpuContext::Reset()
{
  slots_.clear();
  draws_.clear();
  textures_.clear();
  clear_rgba_ = 0;
  frames_presented_ = 0;
}

} // namespace neko::compositor
