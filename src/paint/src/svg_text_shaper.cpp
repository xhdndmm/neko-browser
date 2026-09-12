#include "neko/paint/svg_text_shaper.h"

#include "neko/graphics/font_face.h"
#include "neko/graphics/font_registry.h"
#include "neko/graphics/font_selector.h"
#include "neko/graphics/utf8.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace neko::paint {
namespace {

struct SelectorKey
{
  std::string family;
  int weight = 400;
  bool italic = false;

  bool operator<(const SelectorKey& other) const
  {
    if (family != other.family) {
      return family < other.family;
    }
    if (weight != other.weight) {
      return weight < other.weight;
    }
    return italic < other.italic;
  }
};

image::SvgOutlineEdge ConvertEdge(const graphics::OutlineEdge& edge)
{
  image::SvgOutlineEdge out;
  switch (edge.kind) {
  case graphics::OutlineKind::kMove:
    out.kind = image::SvgOutlineEdge::kMove;
    break;
  case graphics::OutlineKind::kLine:
    out.kind = image::SvgOutlineEdge::kLine;
    break;
  case graphics::OutlineKind::kQuadratic:
    out.kind = image::SvgOutlineEdge::kQuadratic;
    break;
  case graphics::OutlineKind::kCubic:
    out.kind = image::SvgOutlineEdge::kCubic;
    break;
  case graphics::OutlineKind::kClose:
    out.kind = image::SvgOutlineEdge::kClose;
    break;
  }
  for (int i = 0; i < 6; ++i) {
    out.p[static_cast<std::size_t>(i)] = static_cast<double>(edge.p[i]);
  }
  return out;
}

} // namespace

image::SvgTextShaper CreateSvgTextShaper(const graphics::FontRegistry& fonts)
{
  // Selector cache: FontSelector construction scans the system font stack, and
  // an SVG image may contain hundreds of <text> nodes.
  auto cache = std::make_shared<std::map<SelectorKey, std::unique_ptr<graphics::FontSelector>>>();
  auto cache_mutex = std::make_shared<std::mutex>();

  return [&fonts, cache, cache_mutex](std::string_view family,
                                      bool bold,
                                      bool italic,
                                      double pixel_size,
                                      std::string_view text,
                                      std::vector<image::SvgGlyphOutline>& out) {
    if (pixel_size <= 0.0) {
      return false;
    }
    const SelectorKey key{std::string(family), bold ? 700 : 400, italic};
    const graphics::FontSelector* selector = nullptr;
    {
      std::lock_guard<std::mutex> lock(*cache_mutex);
      auto it = cache->find(key);
      if (it == cache->end()) {
        it = cache
                 ->emplace(key,
                           std::make_unique<graphics::FontSelector>(
                               fonts.library(), key.family, key.weight, key.italic))
                 .first;
      }
      selector = it->second.get();
    }
    const graphics::FontFace* primary = selector->PrimaryFace();
    if (primary == nullptr) {
      return false;
    }
    std::vector<uint32_t> code_points;
    graphics::DecodeUtf8(text, code_points);
    out.clear();
    out.reserve(code_points.size());
    const auto px = static_cast<float>(pixel_size);
    for (const uint32_t code_point : code_points) {
      const graphics::FontFace* face = selector->FaceForCodePoint(code_point);
      if (face == nullptr) {
        face = primary;
      }
      const std::optional<graphics::GlyphOutline> outline = face->OutlineGlyph(code_point, px);
      if (!outline.has_value()) {
        return false;
      }
      image::SvgGlyphOutline glyph;
      glyph.advance = static_cast<double>(outline->advance);
      glyph.edges.reserve(outline->edges.size());
      for (const graphics::OutlineEdge& edge : outline->edges) {
        glyph.edges.push_back(ConvertEdge(edge));
      }
      out.push_back(std::move(glyph));
    }
    return true;
  };
}

} // namespace neko::paint
