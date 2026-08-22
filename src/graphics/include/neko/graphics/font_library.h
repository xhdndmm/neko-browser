#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace neko::graphics {

class FontFace;

// Owns the FreeType library instance and the font faces loaded through it.
// Faces are cached by path so repeated loads share one face.
class FontLibrary
{
public:
  FontLibrary();
  ~FontLibrary();

  FontLibrary(const FontLibrary&) = delete;
  FontLibrary& operator=(const FontLibrary&) = delete;

  bool valid() const;

  // Loads (or returns the cached) face for |path|; nullptr when the library is
  // unusable or the file does not parse.  Const-safe: the face cache is a
  // mutable memo.
  const FontFace* LoadFace(const std::string& path) const;

  // Loads (or returns the cached) face from in-memory font bytes (TTF/OTF/
  // WOFF/WOFF2 per FreeType support).  |key| identifies the face (e.g. a
  // URL); the face owns the bytes.  nullptr when the data does not parse.
  const FontFace* LoadFaceFromMemory(const std::string& key, std::vector<uint8_t> data) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  // Mutable so LoadFace() is callable from const contexts (layout/paint
  // receive a const registry).
};

} // namespace neko::graphics
