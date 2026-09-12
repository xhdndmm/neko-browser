// Unit tests for the compositor: Surface blit/scroll semantics and
// SoftwareCompositor layering, opacity, dirty-rect recomposition and scroll
// bands, plus the GPU front end's fallback and device ledger (ADR 0017).
//
// Reference values are computed with the same fixed-point math as
// paint::Rasterizer's BlendPixel (documented in surface.h).

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

// A solid-color RGBA8888 surface of the given size.
Surface SolidSurface(int width, int height, Color c)
{
  Surface s(width, height);
  s.Clear(c);
  return s;
}

// A copy of |c| with an explicit alpha component.
Color ColorWithAlpha(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
{
  return Color{r, g, b, a};
}

const Color kOpaqueRed{255, 0, 0, 255};
const Color kOpaqueGreen{0, 255, 0, 255};
const Color kOpaqueBlue{0, 0, 255, 255};
const Color kTransparent{0, 0, 0, 0};

Color PixelAt(const Surface& s, int x, int y)
{
  const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(s.width()) +
                         static_cast<std::size_t>(x)) *
                        4;
  return Color{s.pixels()[i], s.pixels()[i + 1], s.pixels()[i + 2], s.pixels()[i + 3]};
}

TEST(SurfaceTest, ClearFillsAllPixels)
{
  Surface s(4, 3);
  s.Clear(kOpaqueBlue);
  EXPECT_EQ(s.width(), 4);
  EXPECT_EQ(s.height(), 3);
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 4; ++x) {
      EXPECT_EQ(PixelAt(s, x, y), kOpaqueBlue);
    }
  }
}

TEST(SurfaceTest, ResizeReusesStorage)
{
  Surface s(10, 10);
  s.Clear(kOpaqueRed);
  s.Resize(4, 4); // shrink: storage reused, geometry updated
  EXPECT_EQ(s.width(), 4);
  EXPECT_EQ(s.height(), 4);
  s.Resize(6, 6); // grow: old content still readable at (0,0)
  EXPECT_EQ(PixelAt(s, 0, 0), kOpaqueRed);
}

TEST(SurfaceTest, CopyFromClipsToDestination)
{
  Surface src = SolidSurface(4, 4, kOpaqueRed);
  Surface dst(6, 6);
  dst.Clear(kOpaqueGreen);
  dst.CopyFrom(src, 4, 4); // sticks out the bottom-right corner
  EXPECT_EQ(PixelAt(dst, 0, 0), kOpaqueGreen);
  EXPECT_EQ(PixelAt(dst, 4, 4), kOpaqueRed);
  EXPECT_EQ(PixelAt(dst, 5, 5), kOpaqueRed);
  EXPECT_EQ(PixelAt(dst, 3, 3), kOpaqueGreen);

  dst.CopyFrom(src, -2, -2); // sticks out the top-left corner
  EXPECT_EQ(PixelAt(dst, 0, 0), kOpaqueRed);
  EXPECT_EQ(PixelAt(dst, 1, 1), kOpaqueRed);
  EXPECT_EQ(PixelAt(dst, 2, 2), kOpaqueGreen); // untouched outside src
}

TEST(SurfaceTest, CopyRectCopiesOnlyRequestedRegion)
{
  Surface src(4, 4);
  src.Clear(kOpaqueGreen);
  // Paint a red 2x2 in the middle of the source.
  Surface patch = SolidSurface(2, 2, kOpaqueRed);
  src.CopyRect(patch, 0, 0, 2, 2, 1, 1);

  Surface dst(6, 6);
  dst.Clear(kOpaqueBlue);
  dst.CopyRect(src, 1, 1, 2, 2, 2, 2); // grab only the red patch
  EXPECT_EQ(PixelAt(dst, 2, 2), kOpaqueRed);
  EXPECT_EQ(PixelAt(dst, 3, 3), kOpaqueRed);
  EXPECT_EQ(PixelAt(dst, 1, 1), kOpaqueBlue);
  EXPECT_EQ(PixelAt(dst, 4, 4), kOpaqueBlue);
}

TEST(SurfaceTest, CopyBandCopiesHorizontalStrip)
{
  Surface src = SolidSurface(5, 8, kOpaqueRed);
  Surface dst = SolidSurface(5, 8, kOpaqueGreen);
  dst.CopyBand(src, 3, 5);
  EXPECT_EQ(PixelAt(dst, 0, 2), kOpaqueGreen);
  EXPECT_EQ(PixelAt(dst, 0, 3), kOpaqueRed);
  EXPECT_EQ(PixelAt(dst, 0, 4), kOpaqueRed);
  EXPECT_EQ(PixelAt(dst, 0, 5), kOpaqueGreen);
}

TEST(SurfaceTest, BlendOverUsesSourceAlpha)
{
  Surface src(1, 1);
  src.Clear(Color{100, 0, 0, 128}); // half-transparent red
  Surface dst = SolidSurface(1, 1, Color{0, 100, 0, 255});
  dst.BlendOver(src, 0, 0);
  const Color blended = PixelAt(dst, 0, 0);
  // Red channel rises from 0 toward 100; green falls from 100 toward 0.
  EXPECT_GT(blended.r, 0);
  EXPECT_LT(blended.g, 100);
  // Opaque source keeps the destination opaque.
  EXPECT_EQ(blended.a, 255);
}

TEST(SurfaceTest, BlendOverOpaqueSourceReplacesPixel)
{
  Surface src = SolidSurface(2, 2, kOpaqueRed);
  Surface dst = SolidSurface(2, 2, kOpaqueGreen);
  dst.BlendOver(src, 0, 0);
  EXPECT_EQ(PixelAt(dst, 0, 0), kOpaqueRed);
  EXPECT_EQ(PixelAt(dst, 1, 1), kOpaqueRed);
}

TEST(SurfaceTest, BlendOverOpacityScalesSourceAlpha)
{
  Surface src = SolidSurface(1, 1, Color{255, 255, 255, 255});
  Surface dst = SolidSurface(1, 1, kOpaqueBlue);
  dst.BlendOver(src, 0, 0, /*opacity=*/0.0f);
  EXPECT_EQ(PixelAt(dst, 0, 0), kOpaqueBlue); // zero opacity = no-op
  dst.BlendOver(src, 0, 0, /*opacity=*/1.0f);
  EXPECT_EQ(PixelAt(dst, 0, 0), (Color{255, 255, 255, 255}));
}

TEST(SurfaceTest, BlendOverClipRectRestrictsTouch)
{
  Surface src = SolidSurface(4, 4, kOpaqueRed);
  Surface dst = SolidSurface(4, 4, kOpaqueGreen);
  dst.BlendOver(src, 0, 0, 1.0f, /*clip=*/1, 1, 2, 2);
  EXPECT_EQ(PixelAt(dst, 0, 0), kOpaqueGreen);
  EXPECT_EQ(PixelAt(dst, 1, 1), kOpaqueRed);
  EXPECT_EQ(PixelAt(dst, 2, 2), kOpaqueRed);
  EXPECT_EQ(PixelAt(dst, 3, 3), kOpaqueGreen);
}

TEST(SurfaceTest, ShiftRowsMovesContentDown)
{
  Surface s(2, 4);
  s.Clear(kTransparent);
  s.CopyFrom(SolidSurface(2, 1, kOpaqueRed), 0, 0);
  s.CopyFrom(SolidSurface(2, 1, kOpaqueGreen), 0, 1);
  s.CopyFrom(SolidSurface(2, 1, kOpaqueBlue), 0, 2);
  s.ShiftRows(1);                          // content moves down by one row
  EXPECT_EQ(PixelAt(s, 0, 0), kOpaqueRed); // revealed row untouched
  EXPECT_EQ(PixelAt(s, 0, 1), kOpaqueRed);
  EXPECT_EQ(PixelAt(s, 0, 2), kOpaqueGreen);
  EXPECT_EQ(PixelAt(s, 0, 3), kOpaqueBlue);
}

TEST(SurfaceTest, ShiftRowsMovesContentUp)
{
  Surface s(2, 4);
  s.Clear(kTransparent);
  s.CopyFrom(SolidSurface(2, 1, kOpaqueRed), 0, 0);
  s.CopyFrom(SolidSurface(2, 1, kOpaqueGreen), 0, 1);
  s.CopyFrom(SolidSurface(2, 1, kOpaqueBlue), 0, 2);
  s.CopyFrom(SolidSurface(2, 1, Color{255, 255, 255, 255}), 0, 3);
  s.ShiftRows(-1); // content moves up by one row
  EXPECT_EQ(PixelAt(s, 0, 0), kOpaqueGreen);
  EXPECT_EQ(PixelAt(s, 0, 1), kOpaqueBlue);
  EXPECT_EQ(PixelAt(s, 0, 2), (Color{255, 255, 255, 255}));
  EXPECT_EQ(PixelAt(s, 0, 3), (Color{255, 255, 255, 255})); // revealed row untouched
}

TEST(SoftwareCompositorTest, CompositeCopiesBackdropAndBlendsOverlays)
{
  SoftwareCompositor compositor(4, 4);
  compositor.LayerSurface(0).Resize(4, 4);
  compositor.LayerSurface(0).CopyFrom(SolidSurface(4, 4, kOpaqueBlue));
  // A 2x2 red overlay at (1,1) over the blue page.
  compositor.LayerSurface(1) = SolidSurface(2, 2, kOpaqueRed);
  compositor.SetLayerPlacement(1, 1, 1);
  compositor.Composite(kTransparent);
  const Surface& out = compositor.Output();
  EXPECT_EQ(PixelAt(out, 0, 0), kOpaqueBlue);
  EXPECT_EQ(PixelAt(out, 1, 1), kOpaqueRed);
  EXPECT_EQ(PixelAt(out, 2, 2), kOpaqueRed);
  EXPECT_EQ(PixelAt(out, 3, 3), kOpaqueBlue);
}

TEST(SoftwareCompositorTest, InvisibleOverlayIsSkipped)
{
  SoftwareCompositor compositor(4, 4);
  compositor.LayerSurface(0).Resize(4, 4);
  compositor.LayerSurface(0).CopyFrom(SolidSurface(4, 4, kOpaqueBlue));
  compositor.LayerSurface(1) = SolidSurface(4, 4, kOpaqueRed);
  compositor.SetLayerVisible(1, false);
  compositor.Composite(kTransparent);
  EXPECT_EQ(PixelAt(compositor.Output(), 2, 2), kOpaqueBlue);
}

TEST(SoftwareCompositorTest, OverlayOpacityBlends)
{
  SoftwareCompositor compositor(1, 1);
  compositor.LayerSurface(0).Resize(1, 1);
  compositor.LayerSurface(0).CopyFrom(SolidSurface(1, 1, Color{0, 0, 200, 255}));
  compositor.LayerSurface(1) = SolidSurface(1, 1, Color{200, 0, 0, 255});
  compositor.SetLayerOpacity(1, 0.5f);
  compositor.Composite(kTransparent);
  const Color blended = PixelAt(compositor.Output(), 0, 0);
  EXPECT_GT(blended.r, 0);
  EXPECT_LT(blended.r, 200);
  EXPECT_GT(blended.b, 0);
  EXPECT_LT(blended.b, 200);
}

TEST(SoftwareCompositorTest, CompositeRectRestoresBackdropThenReblends)
{
  SoftwareCompositor compositor(4, 4);
  compositor.LayerSurface(0).Resize(4, 4);
  compositor.LayerSurface(0).CopyFrom(SolidSurface(4, 4, kOpaqueBlue));
  compositor.LayerSurface(1) = SolidSurface(4, 4, kOpaqueRed);
  compositor.Composite(kTransparent);
  EXPECT_EQ(PixelAt(compositor.Output(), 0, 0), kOpaqueRed); // full red overlay

  // Make the overlay transparent at (1,1)-(2,2) and re-composite only that
  // region: the blue backdrop must be restored there.
  compositor.LayerSurface(1) = SolidSurface(4, 4, Color{255, 0, 0, 0});
  compositor.CompositeRect(1, 1, 2, 2);
  EXPECT_EQ(PixelAt(compositor.Output(), 0, 0), kOpaqueRed);  // outside region: stale red
  EXPECT_EQ(PixelAt(compositor.Output(), 1, 1), kOpaqueBlue); // restored backdrop
  EXPECT_EQ(PixelAt(compositor.Output(), 2, 2), kOpaqueBlue);
  EXPECT_EQ(PixelAt(compositor.Output(), 3, 3), kOpaqueRed);
}

TEST(SoftwareCompositorTest, ScrollOutputReportsExposedBandAndKeepsOverlayGlued)
{
  SoftwareCompositor compositor(4, 8);
  compositor.LayerSurface(0) = SolidSurface(4, 8, kOpaqueBlue);
  compositor.LayerSurface(1) = SolidSurface(2, 1, kOpaqueRed);
  compositor.SetLayerPlacement(1, 1, 2); // red strip over row 2
  compositor.Composite(kTransparent);
  EXPECT_EQ(PixelAt(compositor.Output(), 1, 2), kOpaqueRed);

  // Scroll content down by 3 rows: rows [0,3) are exposed, the red overlay
  // moves down with the page (row 5).
  int band_y0 = -1;
  int band_y1 = -1;
  compositor.ScrollOutput(3, &band_y0, &band_y1);
  EXPECT_EQ(band_y0, 0);
  EXPECT_EQ(band_y1, 3);
  EXPECT_EQ(PixelAt(compositor.Output(), 1, 5), kOpaqueRed);
  // Refill the exposed band from the page layer.
  compositor.Output().CopyBand(compositor.LayerSurface(0), band_y0, band_y1);
  EXPECT_EQ(PixelAt(compositor.Output(), 0, 0), kOpaqueBlue);
  EXPECT_EQ(PixelAt(compositor.Output(), 0, 2), kOpaqueBlue);

  // Scroll back up by 2 rows: rows [6,8) are exposed.
  compositor.ScrollOutput(-2, &band_y0, &band_y1);
  EXPECT_EQ(band_y0, 6);
  EXPECT_EQ(band_y1, 8);
  EXPECT_EQ(PixelAt(compositor.Output(), 1, 3), kOpaqueRed); // moved back up
}

TEST(SoftwareCompositorTest, LayerPlacementOffsetComposites)
{
  SoftwareCompositor compositor(6, 6);
  compositor.LayerSurface(0) = SolidSurface(6, 6, kOpaqueBlue);
  compositor.LayerSurface(1) = SolidSurface(2, 2, kOpaqueGreen);
  compositor.SetLayerPlacement(1, -1, -1); // sticks out the top-left corner
  compositor.Composite(kTransparent);
  EXPECT_EQ(PixelAt(compositor.Output(), 0, 0), kOpaqueGreen);
  EXPECT_EQ(PixelAt(compositor.Output(), 1, 1), kOpaqueBlue);
}

// ---------------------------------------------------------------------------
// GPU compositor (ADR 0017).  The device backends are not implemented, so the
// tests pin down what does exist: the software fallback, the capability probe,
// and the upload/draw ledger that a real backend will drive.
// ---------------------------------------------------------------------------

TEST(GpuCompositorTest, ProbeReportsNoBackendWithoutDevice)
{
  const GpuCapabilities caps = ProbeGpuCapabilities();
  // No platform backend is wired yet; reporting availability would make the
  // compositor take a path that cannot present.
  EXPECT_FALSE(caps.available);
  EXPECT_EQ(caps.backend, GpuBackend::None);
}

TEST(GpuCompositorTest, CreateFallsBackToSoftware)
{
  auto compositor = GpuCompositor::Create(8, 4);
  ASSERT_NE(compositor, nullptr);
  // The fallback is a real software compositor, not a broken GPU stand-in.
  auto* software = dynamic_cast<SoftwareCompositor*>(compositor.get());
  EXPECT_NE(software, nullptr);
  EXPECT_EQ(compositor->Output().width(), 8);
  EXPECT_EQ(compositor->Output().height(), 4);

  auto forced = GpuCompositor::Create(2, 2, /*force_software=*/true);
  EXPECT_NE(dynamic_cast<SoftwareCompositor*>(forced.get()), nullptr);
}

TEST(GpuCompositorTest, BackendNamesAreStable)
{
  EXPECT_STREQ(GpuBackendName(GpuBackend::None), "None");
  EXPECT_STREQ(GpuBackendName(GpuBackend::OpenGL), "OpenGL");
  EXPECT_STREQ(GpuBackendName(GpuBackend::Vulkan), "Vulkan");
  EXPECT_STREQ(GpuBackendName(GpuBackend::Metal), "Metal");
  EXPECT_STREQ(GpuBackendName(GpuBackend::Direct3D), "Direct3D");
}

TEST(GpuCompositorTest, RecordingContextReceivesLayerUploadsAndDraws)
{
  auto context = std::make_unique<RecordingGpuContext>();
  auto* recorder = context.get();
  auto compositor = GpuCompositor::CreateWithContext(4, 4, std::move(context));
  ASSERT_TRUE(compositor->using_gpu());

  compositor->LayerSurface(0) = SolidSurface(4, 4, kOpaqueBlue);
  compositor->LayerSurface(1) = SolidSurface(2, 2, kOpaqueRed);
  compositor->SetLayerPlacement(1, 1, 1);
  compositor->Composite(kTransparent);

  // One texture per distinct layer, uploaded in full.
  ASSERT_EQ(recorder->textures().size(), 2u);
  EXPECT_EQ(recorder->textures()[0].width, 4);
  EXPECT_EQ(recorder->textures()[0].height, 4);
  EXPECT_EQ(recorder->textures()[1].width, 2);
  EXPECT_EQ(recorder->textures()[1].height, 2);
  EXPECT_EQ(compositor->texture_uploads(), 2);

  // One textured quad per visible layer, at the layer's placement.
  ASSERT_EQ(recorder->draws().size(), 2u);
  EXPECT_EQ(recorder->draws()[0].x, 0.0f);
  EXPECT_EQ(recorder->draws()[0].y, 0.0f);
  EXPECT_EQ(recorder->draws()[1].x, 1.0f);
  EXPECT_EQ(recorder->draws()[1].y, 1.0f);
  EXPECT_EQ(compositor->draw_calls(), 2);
  EXPECT_EQ(recorder->frames_presented(), 1);

  // The uploaded texture holds the layer's pixels.
  const auto& pixels = recorder->textures()[1].pixels;
  ASSERT_EQ(pixels.size(), 2u * 2u * 4u);
  EXPECT_EQ(pixels[0], kOpaqueRed.r);
  EXPECT_EQ(pixels[1], kOpaqueRed.g);
  EXPECT_EQ(pixels[2], kOpaqueRed.b);
  EXPECT_EQ(pixels[3], kOpaqueRed.a);
}

TEST(GpuCompositorTest, GpuPathOutputMatchesSoftwareReference)
{
  // The presented surface must be pixel-identical to what the software
  // compositor produces: the GPU path is an acceleration, not a behavior.
  SoftwareCompositor reference(6, 6);
  reference.LayerSurface(0) = SolidSurface(6, 6, kOpaqueBlue);
  reference.LayerSurface(1) =
      SolidSurface(4, 4, ColorWithAlpha(kOpaqueGreen.r, kOpaqueGreen.g, kOpaqueGreen.b, 128));
  reference.SetLayerPlacement(1, 1, 1);
  reference.Composite(kTransparent);

  auto compositor = GpuCompositor::CreateWithContext(6, 6, std::make_unique<RecordingGpuContext>());
  compositor->LayerSurface(0) = SolidSurface(6, 6, kOpaqueBlue);
  compositor->LayerSurface(1) =
      SolidSurface(4, 4, ColorWithAlpha(kOpaqueGreen.r, kOpaqueGreen.g, kOpaqueGreen.b, 128));
  compositor->SetLayerPlacement(1, 1, 1);
  compositor->Composite(kTransparent);

  ASSERT_EQ(compositor->Output().pixels(), reference.Output().pixels());
}

TEST(GpuCompositorTest, DirtyLayersUploadOnceUntilInvalidated)
{
  auto context = std::make_unique<RecordingGpuContext>();
  auto* recorder = context.get();
  auto compositor = GpuCompositor::CreateWithContext(2, 2, std::move(context));
  compositor->LayerSurface(0) = SolidSurface(2, 2, kOpaqueBlue);

  compositor->Composite(kTransparent);
  EXPECT_EQ(compositor->texture_uploads(), 1);

  // No layer write between frames: the texture stays valid, no re-upload.
  compositor->Composite(kTransparent);
  EXPECT_EQ(compositor->texture_uploads(), 1);
  EXPECT_EQ(recorder->frames_presented(), 2);

  // Writing the layer marks it dirty again.
  compositor->LayerSurface(0) = SolidSurface(2, 2, kOpaqueRed);
  compositor->Composite(kTransparent);
  EXPECT_EQ(compositor->texture_uploads(), 2);
}

TEST(GpuCompositorTest, CompositeRectBlendsOverlaysAndKeepsBackdrop)
{
  auto compositor = GpuCompositor::CreateWithContext(6, 6, std::make_unique<RecordingGpuContext>());
  compositor->LayerSurface(0) = SolidSurface(6, 6, kOpaqueBlue);
  compositor->LayerSurface(1) = SolidSurface(2, 2, kOpaqueGreen);
  compositor->SetLayerPlacement(1, 2, 2);
  compositor->Composite(kTransparent);

  compositor->CompositeRect(2, 2, 2, 2);
  EXPECT_EQ(PixelAt(compositor->Output(), 2, 2), kOpaqueGreen);
  EXPECT_EQ(PixelAt(compositor->Output(), 0, 0), kOpaqueBlue);
}

TEST(GpuCompositorTest, ScrollReportsExposedBandLikeSoftware)
{
  auto compositor = GpuCompositor::CreateWithContext(4, 8, std::make_unique<RecordingGpuContext>());
  compositor->LayerSurface(0) = SolidSurface(4, 8, kOpaqueBlue);
  compositor->Composite(kTransparent);

  int band_y0 = -1;
  int band_y1 = -1;
  compositor->ScrollOutput(3, &band_y0, &band_y1);
  EXPECT_EQ(band_y0, 0);
  EXPECT_EQ(band_y1, 3);

  compositor->ScrollOutput(-2, &band_y0, &band_y1);
  EXPECT_EQ(band_y0, 6);
  EXPECT_EQ(band_y1, 8);
}

TEST(GpuCompositorTest, NullContextReportsNotImplemented)
{
  // The no-device context satisfies the interface but refuses device work.
  NullGpuContext null_context;
  EXPECT_FALSE(null_context.hardware_accelerated());
  std::uintptr_t handle = 0;
  const auto created = null_context.CreateTexture(2, 2, &handle);
  EXPECT_FALSE(created.has_value());
  EXPECT_EQ(created.error().category(), base::ErrorCategory::kNotImplemented);
  const auto presented = null_context.EndFrame();
  EXPECT_FALSE(presented.has_value());
}

} // namespace
} // namespace neko::compositor
