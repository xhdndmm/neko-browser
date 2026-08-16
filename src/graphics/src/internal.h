#pragma once

// Internal to neko::graphics: shared FreeType library instance.  Not part of
// the public API (lives in src/, not include/).

#include <ft2build.h>
#include FT_FREETYPE_H

namespace neko::graphics {

// Process-wide FreeType library, initialized on first use.  The rendering
// pipeline is single-threaded, so one library is safe.  Never freed: the
// process owns it for its lifetime.
FT_Library SharedFreeTypeLibrary();

} // namespace neko::graphics
