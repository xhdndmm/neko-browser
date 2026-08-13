#pragma once

#include <memory>
#include <string>

namespace neko::graphics {

class FontFace;

// Owns the FreeType library instance and the font faces loaded through it.
// Faces are cached by path so repeated loads share one face.
class FontLibrary {
 public:
  FontLibrary();
  ~FontLibrary();

  FontLibrary(const FontLibrary&) = delete;
  FontLibrary& operator=(const FontLibrary&) = delete;

  bool valid() const;

  // Loads (or returns the cached) face for |path|; nullptr when the library is
  // unusable or the file does not parse.
  const FontFace* LoadFace(const std::string& path);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace neko::graphics
