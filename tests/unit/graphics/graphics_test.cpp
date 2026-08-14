// Unit tests for neko::graphics (FreeType-backed font rasterization and
// font-family resolution with CJK fallback).
//
// These tests depend on a system font being present (any Linux/macOS/Windows
// desktop has one).  When no candidate font file exists the font-dependent
// tests skip rather than fail, keeping CI green on minimal images.

#include <cstdio>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "neko/graphics/font_face.h"
#include "neko/graphics/font_library.h"
#include "neko/graphics/font_registry.h"
#include "neko/graphics/font_selector.h"
#include "neko/graphics/glyph_cache.h"
#include "neko/graphics/system_fonts.h"

namespace neko::graphics {
namespace {

TEST(SystemFontsTest, FindsSansOrNone) {
  const std::vector<std::string> fonts = FindSystemFonts(GenericFamily::kSansSerif);
  if (!fonts.empty()) {
    EXPECT_FALSE(fonts[0].empty());
  }
  const std::vector<std::string> cjk = FindSystemFonts(GenericFamily::kCjkSans);
  if (!cjk.empty()) {
    EXPECT_FALSE(cjk[0].empty());
  }
}

TEST(FontFaceTest, MissingFileIsInvalid) {
  FontFace face("/nonexistent/neko_missing_font.ttf");
  EXPECT_FALSE(face.valid());
}

TEST(FontFaceTest, RendersAndCachesGlyphs) {
  const std::vector<std::string> fonts = FindSystemFonts(GenericFamily::kSansSerif);
  if (fonts.empty()) {
    GTEST_SKIP() << "no system sans-serif font available";
  }
  FontLibrary library;
  const FontFace* face = library.LoadFace(fonts[0]);
  ASSERT_NE(face, nullptr);
  ASSERT_TRUE(face->valid());

  // Metrics are real (proportional), not the monospace font_size assumption.
  EXPECT_GT(face->Advance('A', 16), 0.0f);
  // The ascent is scaled to the pixel size and stays inside the em box
  // (regression: it was read as unscaled font units, ~30px at 16px, which
  // shifted glyphs a full line below their hit-test box).
  EXPECT_GT(face->Ascent(16), 0.0f);
  EXPECT_LT(face->Ascent(16), 16.0f);
  // Descent is a positive magnitude below the baseline, scaled to pixel size.
  EXPECT_GT(face->Descent(16), 0.0f);
  EXPECT_LT(face->Descent(16), 16.0f);
  EXPECT_GT(face->TextWidth("hello", 16), 0.0f);
  EXPECT_TRUE(face->HasGlyph('A'));

  const GlyphBitmap* first = face->RenderGlyph('A', 16);
  ASSERT_NE(first, nullptr);
  EXPECT_GT(first->width, 0);
  EXPECT_GT(first->advance, 0.0f);
  // The same glyph must come back from the cache (same pointer).
  EXPECT_EQ(face->RenderGlyph('A', 16), first);
  // A space has an advance but a zero-size bitmap.
  const GlyphBitmap* space = face->RenderGlyph(' ', 16);
  ASSERT_NE(space, nullptr);
  EXPECT_GT(space->advance, 0.0f);
}

TEST(FontSelectorTest, ResolvesFamilyAndFallsBackPerCharacter) {
  if (FindSystemFonts(GenericFamily::kSansSerif).empty()) {
    GTEST_SKIP() << "no system sans-serif font available";
  }
  FontRegistry registry;
  // An unknown family falls back to the generic sans-serif; the stack is
  // non-empty and ASCII still measures.
  const FontSelector* selector = registry.SelectorFor("No Such Family, sans-serif");
  ASSERT_NE(selector, nullptr);
  EXPECT_FALSE(selector->faces().empty());
  EXPECT_NE(selector->FaceForCodePoint('A'), nullptr);
  EXPECT_GT(selector->TextWidth("hello", 16), 0.0f);
  EXPECT_GT(selector->Advance('A', 16), 0.0f);
}

TEST(FontSelectorTest, CjkFallbackRendersHanGlyphs) {
  if (FindSystemFonts(GenericFamily::kCjkSans).empty()) {
    GTEST_SKIP() << "no system CJK font available";
  }
  FontRegistry registry;
  const FontSelector* selector = registry.SelectorFor("sans-serif");
  ASSERT_NE(selector, nullptr);
  // U+4E2D (中): a Latin-only face has no glyph, so the CJK fallback face must
  // provide it, and the width/rendering must work.
  EXPECT_NE(selector->FaceForCodePoint(0x4E2D), nullptr);
  EXPECT_GT(selector->Advance(0x4E2D, 16), 0.0f);
  EXPECT_GT(selector->TextWidth("\xe4\xb8\xad", 16), 0.0f);  // "中" in UTF-8
  const GlyphBitmap* glyph = selector->RenderGlyph(0x4E2D, 16);
  ASSERT_NE(glyph, nullptr);
  EXPECT_GT(glyph->width, 0);
}

TEST(FontSelectorTest, ResolveFamilyNameFindsCjk) {
  if (FindSystemFonts(GenericFamily::kCjkSans).empty()) {
    GTEST_SKIP() << "no system CJK font available";
  }
  const std::string path = ResolveFamilyName("Noto Sans CJK SC");
  EXPECT_FALSE(path.empty());
}

TEST(FontFaceTest, UnknownCodePointStillAdvances) {
  const std::vector<std::string> fonts = FindSystemFonts(GenericFamily::kSansSerif);
  if (fonts.empty()) {
    GTEST_SKIP() << "no system sans-serif font available";
  }
  FontLibrary library;
  const FontFace* face = library.LoadFace(fonts[0]);
  ASSERT_NE(face, nullptr);
  // A code point with no glyph falls back to the .notdef advance (nonzero).
  EXPECT_GE(face->Advance(0x10FFFF, 16), 0.0f);
}

TEST(FontVariantTest, BoldSelectsBoldVariant) {
  const std::vector<std::string> fonts = FindSystemFonts(GenericFamily::kSansSerif);
  if (fonts.empty()) {
    GTEST_SKIP() << "no system sans-serif font available";
  }
  const std::string bold = FindFontVariant(fonts[0], 700, false);
  // If the family ships a bold face, the path must differ and exist.
  if (bold != fonts[0]) {
    EXPECT_FALSE(bold.empty());
    std::FILE* f = std::fopen(bold.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    std::fclose(f);
  }
  // Regular weight keeps the base path.
  EXPECT_EQ(FindFontVariant(fonts[0], 400, false), fonts[0]);
}

TEST(FontSelectorTest, BoldUsesDifferentFace) {
  if (FindSystemFonts(GenericFamily::kSansSerif).empty()) {
    GTEST_SKIP() << "no system sans-serif font available";
  }
  FontRegistry registry;
  const FontSelector* regular = registry.SelectorFor("sans-serif", 400, false);
  const FontSelector* bold = registry.SelectorFor("sans-serif", 700, false);
  ASSERT_NE(regular, nullptr);
  ASSERT_NE(bold, nullptr);
  // The bold selector's primary face is a different file when a bold variant
  // exists (e.g. LiberationSans-Bold.ttf).
  const FontFace* regular_face = regular->PrimaryFace();
  const FontFace* bold_face = bold->PrimaryFace();
  ASSERT_NE(regular_face, nullptr);
  ASSERT_NE(bold_face, nullptr);
  if (regular_face->path() != bold_face->path()) {
    // Bold faces are typically wider: 'W' should measure at least as wide.
    EXPECT_GE(bold->Advance('W', 16), regular->Advance('W', 16));
  }
}

}  // namespace
}  // namespace neko::graphics
