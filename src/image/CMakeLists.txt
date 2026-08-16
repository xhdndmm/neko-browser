# =============================================================================
# neko_image — image decoding (Phase 6/9 preview).
#
# PNG and GIF are parsed and decoded in-house (chunks, CRC, filters, Adam7,
# LZW); zlib is used only for the PNG IDAT deflate stream.  JPEG is decoded
# via libjpeg and wrapped behind the Image interface.  All are exposed
# through a single DecodeImage() entry point with format auto-detection.
# =============================================================================

add_library(neko_image
  src/image.cpp
  src/png_decoder.cpp
  src/jpeg_decoder.cpp
  src/gif_decoder.cpp
  src/webp_decoder.cpp
  src/avif_decoder.cpp
  src/svg_decoder.cpp
)
add_library(neko::image ALIAS neko_image)

target_compile_features(neko_image PUBLIC cxx_std_20)
target_include_directories(neko_image PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
find_package(WebP REQUIRED)
find_package(AVIF REQUIRED)
target_link_libraries(neko_image PUBLIC neko::base ZLIB::ZLIB JPEG::JPEG WebP::webp AVIF::avif)
apply_compiler_warnings(neko_image)
