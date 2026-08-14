#include "neko/graphics/font_selector.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "neko/graphics/font_face.h"
#include "neko/graphics/font_library.h"
#include "neko/graphics/system_fonts.h"
#include "neko/graphics/utf8.h"

namespace neko::graphics {
namespace {

std::string Trim(std::string_view s) {
  std::size_t begin = 0;
  std::size_t end = s.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(s[begin])) != 0) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
    --end;
  }
  return std::string(s.substr(begin, end - begin));
}

std::string Lower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

// Strips surrounding quotes from a CSS family name ("Noto Sans" or 'Noto').
std::string Unquote(std::string_view s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    return std::string(s.substr(1, s.size() - 2));
  }
  return std::string(s);
}

}  // namespace

FontSelector::FontSelector(const FontLibrary& library, std::string_view family, int weight,
                           bool italic)
    : library_(library), weight_(weight), italic_(italic) {
  // Split the CSS family list on commas.
  std::size_t start = 0;
  while (start <= family.size()) {
    const std::size_t comma = family.find(',', start);
    const std::string_view part = family.substr(
        start, comma == std::string_view::npos ? family.size() - start : comma - start);
    AddFamily(library_, Trim(part));
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }

  // Chinese fallback: append the platform CJK face(s) if not already present,
  // so Han characters always find a glyph.
  const std::vector<std::string> cjk_paths = FindSystemFonts(GenericFamily::kCjkSans);
  for (const std::string& path : cjk_paths) {
    const FontFace* face = library_.LoadFace(path);
    if (face == nullptr) {
      continue;
    }
    const bool already = std::any_of(faces_.begin(), faces_.end(),
                                     [&](const FontFace* f) { return f->path() == path; });
    if (!already) {
      faces_.push_back(face);
    }
  }
}

void FontSelector::AddFamily(const FontLibrary& library, std::string_view family_name) {
  if (family_name.empty()) {
    return;
  }
  const std::string key = Lower(Unquote(family_name));
  GenericFamily generic;
  bool is_generic = true;
  if (key == "sans-serif" || key == "cursive" || key == "fantasy") {
    generic = GenericFamily::kSansSerif;
  } else if (key == "serif") {
    generic = GenericFamily::kSerif;
  } else if (key == "monospace") {
    generic = GenericFamily::kMonospace;
  } else {
    is_generic = false;
  }

  std::vector<std::string> paths;
  if (is_generic) {
    paths = FindSystemFonts(generic);
  } else {
    const std::string resolved = ResolveFamilyName(Unquote(family_name));
    if (!resolved.empty()) {
      paths.push_back(resolved);
    }
  }
  for (const std::string& path : paths) {
    // Pick the bold/italic variant when requested and available.
    const std::string selected =
        (weight_ >= 600 || italic_) ? FindFontVariant(path, weight_, italic_) : path;
    if (const FontFace* face = library.LoadFace(selected)) {
      faces_.push_back(face);
    }
  }
}

const FontFace* FontSelector::FaceForCodePoint(uint32_t code_point) const {
  for (const FontFace* face : faces_) {
    if (face->HasGlyph(code_point)) {
      return face;
    }
  }
  return faces_.empty() ? nullptr : faces_[0];  // last resort: .notdef
}

float FontSelector::TextWidth(std::string_view text, float px_size) const {
  std::vector<uint32_t> code_points;
  DecodeUtf8(text, code_points);
  float width = 0;
  for (const uint32_t code_point : code_points) {
    width += Advance(code_point, px_size);
  }
  return width;
}

float FontSelector::Advance(uint32_t code_point, float px_size) const {
  const FontFace* face = FaceForCodePoint(code_point);
  return face != nullptr ? face->Advance(code_point, px_size) : 0.0f;
}

const GlyphBitmap* FontSelector::RenderGlyph(uint32_t code_point, float px_size) const {
  const FontFace* face = FaceForCodePoint(code_point);
  return face != nullptr ? face->RenderGlyph(code_point, px_size) : nullptr;
}

float FontSelector::Ascent(float px_size) const {
  const FontFace* face = PrimaryFace();
  return face != nullptr ? face->Ascent(px_size) : 0.0f;
}

float FontSelector::Descent(float px_size) const {
  const FontFace* face = PrimaryFace();
  return face != nullptr ? face->Descent(px_size) : 0.0f;
}

}  // namespace neko::graphics
