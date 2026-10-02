// Device-backed GPU compositor tests (ADR 0017): real EGL/OpenGL contexts,
// pixel parity with the software reference, sub-rectangle uploads and
// readback.  Every test skips itself when no GPU backend can be created, so
// the suite stays runnable on CI machines without a graphics stack.

#include "neko/compositor/compositor.h"
#include "neko/compositor/gpu_compositor.h"
#include "neko/compositor/gpu_context.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

namespace neko::compositor {
namespace {

using neko::css::Color;

// Builds the shared parity scene on any Compositor implementation.  It
// exercises every path the composition shader must replicate: layer 0 raw
// copies (including translucent pixels), straight-alpha blending, negative
// placement clipping, layer opacity, and an invisible layer.
void BuildParityScene(Compositor& compositor)
{
  Surface& page = compositor.LayerSurface(0);
  page.Resize(8, 6);
  page.Clear(Color{0, 0, 0, 255});
  for (int y = 0; y < 6; ++y) {
    for (int x = 0; x < 8; ++x) {
      std::uint8_t* p =
          &page.pixels()[(static_cast<std::size_t>(y) * 8 + static_cast<std::size_t>(x)) * 4];
      p[0] = static_cast<std::uint8_t>(20 + x * 25);
      p[1] = static_cast<std::uint8_t>(30 + y * 30);
      p[2] = static_cast<std::uint8_t>(40 + ((x * 7 + y * 13) % 200));
      p[3] = ((x + y) % 3 == 0) ? 128 : 255;
    }
  }

  Surface& overlay = compositor.LayerSurface(1);
  overlay.Resize(4, 3);
  overlay.Clear(Color{0, 200, 80, 128});
  compositor.SetLayerPlacement(1, 3, 2);

  Surface& second = compositor.LayerSurface(2);
  second.Resize(3, 4);
  second.Clear(Color{200, 0, 160, 200});
  compositor.SetLayerPlacement(2, -1, 4);
  compositor.SetLayerOpacity(2, 0.5f);

  Surface& hidden = compositor.LayerSurface(3);
  hidden.Resize(2, 2);
  hidden.Clear(Color{255, 255, 0, 255});
  compositor.SetLayerVisible(3, false);

  compositor.Composite(Color{10, 20, 30, 40});
}

TEST(GpuBackendTest, CompositionMatchesSoftwarePixelForPixel)
{
  if (!CreateBestGpuContext()) {
    GTEST_SKIP() << "no GPU backend on this machine";
  }

  SoftwareCompositor reference(8, 6);
  BuildParityScene(reference);

  std::unique_ptr<GpuContext> context = CreateBestGpuContext();
  ASSERT_NE(context, nullptr);
  GpuContext* device = context.get();
  auto compositor = GpuCompositor::CreateWithContext(8, 6, std::move(context));
  ASSERT_TRUE(compositor->using_gpu());
  BuildParityScene(*compositor);

  std::vector<std::uint8_t> rendered;
  ASSERT_TRUE(device->Readback(&rendered).has_value());
  EXPECT_EQ(rendered, reference.Output().pixels());
}

TEST(GpuBackendTest, ReadbackReflectsSubRectangleUploads)
{
  std::unique_ptr<GpuContext> context = CreateBestGpuContext();
  if (!context) {
    GTEST_SKIP() << "no GPU backend on this machine";
  }
  ASSERT_TRUE(context->ResizeSurface(4, 4).has_value());

  std::uintptr_t handle = 0;
  ASSERT_TRUE(context->CreateTexture(4, 4, &handle).has_value());

  // A 2x2 opaque-green patch with a 2-pixel row pitch, uploaded at (1, 1).
  const std::vector<std::uint8_t> patch = {
      0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
  ASSERT_TRUE(context->UploadTexture(handle, patch.data(), 8, 1, 1, 2, 2).has_value());

  context->BeginFrame(0x00000000u);
  GpuDrawCall call;
  call.texture = handle;
  call.x = 0.0f;
  call.y = 0.0f;
  call.width = 4.0f;
  call.height = 4.0f;
  call.opacity = 1.0f;
  call.blend = GpuBlendMode::kCopy;
  context->Draw(call);
  ASSERT_TRUE(context->EndFrame().has_value());

  std::vector<std::uint8_t> pixels;
  ASSERT_TRUE(context->Readback(&pixels).has_value());

  Surface expected(4, 4);
  expected.Clear(Color{0, 0, 0, 0});
  Surface green(2, 2);
  green.Clear(Color{0, 255, 0, 255});
  expected.CopyRect(green, 0, 0, 2, 2, 1, 1);
  EXPECT_EQ(pixels, expected.pixels());
}

TEST(GpuBackendTest, EmptyFrameReadsBackAsBackground)
{
  std::unique_ptr<GpuContext> context = CreateBestGpuContext();
  if (!context) {
    GTEST_SKIP() << "no GPU backend on this machine";
  }
  ASSERT_TRUE(context->ResizeSurface(3, 2).has_value());
  context->BeginFrame(0x11223344u);
  ASSERT_TRUE(context->EndFrame().has_value());

  std::vector<std::uint8_t> pixels;
  ASSERT_TRUE(context->Readback(&pixels).has_value());
  ASSERT_EQ(pixels.size(), 3u * 2u * 4u);
  for (std::size_t i = 0; i < pixels.size(); i += 4) {
    EXPECT_EQ(pixels[i + 0], 0x11);
    EXPECT_EQ(pixels[i + 1], 0x22);
    EXPECT_EQ(pixels[i + 2], 0x33);
    EXPECT_EQ(pixels[i + 3], 0x44);
  }
}

TEST(GpuBackendTest, ReadbackRequiresAPresentedFrame)
{
  std::unique_ptr<GpuContext> context = CreateBestGpuContext();
  if (!context) {
    GTEST_SKIP() << "no GPU backend on this machine";
  }
  std::vector<std::uint8_t> pixels;
  EXPECT_FALSE(context->Readback(&pixels).has_value());
}

TEST(GpuBackendTest, IdentityAndLimitsAreReported)
{
  std::unique_ptr<GpuContext> context = CreateBestGpuContext();
  if (!context) {
    GTEST_SKIP() << "no GPU backend on this machine";
  }
  EXPECT_EQ(context->backend(), GpuBackend::OpenGL);
  EXPECT_FALSE(context->renderer().empty());
  EXPECT_FALSE(context->vendor().empty());
  EXPECT_GT(context->max_texture_size(), 0);
}

} // namespace
} // namespace neko::compositor
