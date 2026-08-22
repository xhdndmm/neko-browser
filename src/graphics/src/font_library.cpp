#include "neko/graphics/font_library.h"

#include "neko/graphics/font_face.h"

#include "internal.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace neko::graphics {

struct FontLibrary::Impl
{
  mutable std::unordered_map<std::string, std::unique_ptr<FontFace>> faces;
  mutable std::mutex mutex;
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
  std::lock_guard<std::mutex> lock(impl_->mutex);
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

const FontFace* FontLibrary::LoadFaceFromMemory(const std::string& key,
                                                std::vector<uint8_t> data) const
{
  if (!valid() || data.empty()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  // A second registration of the same key keeps the first face (its bytes).
  const auto it = impl_->faces.find(key);
  if (it != impl_->faces.end()) {
    return it->second.get();
  }
  auto face = std::make_unique<FontFace>(key, std::move(data));
  if (!face->valid()) {
    return nullptr;
  }
  const FontFace* raw = face.get();
  impl_->faces.emplace(key, std::move(face));
  return raw;
}

} // namespace neko::graphics
