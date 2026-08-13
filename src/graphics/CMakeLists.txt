# =============================================================================
# neko_graphics — font rasterization (FreeType), the only module allowed to
# touch FreeType API types (dependency policy §3, ADR 0009).
#
# FontLibrary owns the FreeType library instance; FontFace wraps one font file
# (metrics + glyph rasterization); GlyphCache memoizes rasterized glyphs;
# system font discovery returns candidate font file paths per generic family.
# =============================================================================

add_library(neko_graphics
  src/internal.cpp
  src/font_library.cpp
  src/font_face.cpp
  src/glyph_cache.cpp
  src/system_fonts.cpp
  src/utf8.cpp
)
add_library(neko::graphics ALIAS neko_graphics)

target_compile_features(neko_graphics PUBLIC cxx_std_20)
target_include_directories(neko_graphics PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_graphics PUBLIC Freetype::Freetype neko::base)
apply_compiler_warnings(neko_graphics)
