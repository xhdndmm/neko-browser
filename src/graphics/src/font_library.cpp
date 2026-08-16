#include "neko/graphics/font_library.h"

#include "neko/graphics/font_face.h"

#include "internal.h"

#include <string>
#include <unordered_map>
#include <utility>

namespace neko::graphics {

struct FontLibrary::Impl
{
  mutable std::unordered_map<std::string, std::unique_ptr<FontFace>> faces;
};

FontLibrary::FontLibrary() : impl_(new Impl) {}

FontLibrary::~FontLibrary() = default;

bool FontLibrary::valid() const
{
  return SharedFreeTypeLibrary() != nullptr;
}

const FontFace* FontLibrary::LoadFace(const std::string& path) const
{
  if (!valid()) {
    return nullptr;
  }
  const auto it = impl_->faces.find(path);
  if (it != impl_->faces.end()) {
    return it->second.get();
  }
  auto face = std::make_unique<FontFace>(path);
  if (!face->valid()) {
    return nullptr;
  }
  const FontFace* raw = face.get();
  impl_->faces.emplace(path, std::move(face));
  return raw;
}

} // namespace neko::graphics
