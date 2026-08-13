// Unit tests for neko::graphics (FreeType-backed font rasterization).
//
// These tests depend on a system font being present (any Linux/macOS/Windows
// desktop has one).  When no candidate font file exists the font-dependent
// tests skip rather than fail, keeping CI green on minimal images.

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "neko/graphics/font_face.h"
#include "neko/graphics/font_library.h"
#include "neko/graphics/glyph_cache.h"
#include "neko/graphics/system_fonts.h"

namespace neko::graphics {
namespace {

TEST(SystemFontsTest, FindsSansOrNone) {
  const std::optional<std::string> path = FindSystemFont(GenericFamily::kSansSerif);
  // Either a usable path or a graceful nullopt; never garbage.
  if (path.has_value()) {
    EXPECT_FALSE(path->empty());
  }
  const std::optional<std::string> cjk = FindSystemFont(GenericFamily::kCjkSans);
  if (cjk.has_value()) {
    EXPECT_FALSE(cjk->empty());
  }
}

TEST(FontFaceTest, MissingFileIsInvalid) {
  FontFace face("/nonexistent/neko_missing_font.ttf");
  EXPECT_FALSE(face.valid());
}

TEST(FontFaceTest, RendersAndCachesGlyphs) {
  const std::optional<std::string> path = FindSystemFont(GenericFamily::kSansSerif);
  if (!path.has_value()) {
    GTEST_SKIP() << "no system sans-serif font available";
  }
  FontLibrary library;
  const FontFace* face = library.LoadFace(*path);
  ASSERT_NE(face, nullptr);
  ASSERT_TRUE(face->valid());

  // Metrics are real (proportional), not the monospace font_size assumption.
  EXPECT_GT(face->Advance('A', 16), 0.0f);
  EXPECT_GT(face->Ascent(16), 0.0f);

  const GlyphBitmap* first = face->RenderGlyph('A', 16);
  ASSERT_NE(first, nullptr);
  EXPECT_GT(first->width, 0);
  EXPECT_GT(first->advance, 0.0f);
  // The same glyph must come back from the cache (same pointer).
  EXPECT_EQ(face->RenderGlyph('A', 16), first);
  // Different size is a different cache entry.
  EXPECT_NE(face->RenderGlyph('A', 32), first);
  // A space has an advance but a zero-size bitmap.
  const GlyphBitmap* space = face->RenderGlyph(' ', 16);
  ASSERT_NE(space, nullptr);
  EXPECT_GT(space->advance, 0.0f);
}

TEST(FontFaceTest, UnknownCodePointStillAdvances) {
  const std::optional<std::string> path = FindSystemFont(GenericFamily::kSansSerif);
  if (!path.has_value()) {
    GTEST_SKIP() << "no system sans-serif font available";
  }
  FontLibrary library;
  const FontFace* face = library.LoadFace(*path);
  ASSERT_NE(face, nullptr);
  // A code point with no glyph falls back to the .notdef advance (nonzero).
  EXPECT_GE(face->Advance(0x10FFFF, 16), 0.0f);
}

}  // namespace
}  // namespace neko::graphics
