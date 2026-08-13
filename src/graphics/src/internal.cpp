#include "internal.h"

#include <ft2build.h>
#include FT_FREETYPE_H

namespace neko::graphics {

FT_Library SharedFreeTypeLibrary() {
  static FT_Library library = [] {
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) {
      return static_cast<FT_Library>(nullptr);
    }
    return lib;
  }();
  return library;
}

}  // namespace neko::graphics
