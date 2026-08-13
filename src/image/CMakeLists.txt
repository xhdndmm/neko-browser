# =============================================================================
# neko_image — image decoding (Phase 6/9 preview).
#
# PNG is parsed and decoded in-house (chunks, CRC, filters, Adam7); zlib is
# used only for the IDAT deflate stream.  JPEG is decoded via libjpeg and
# wrapped behind the Image interface.  Both are exposed through a single
# DecodeImage() entry point with format auto-detection.
# =============================================================================

add_library(neko_image
  src/image.cpp
  src/png_decoder.cpp
  src/jpeg_decoder.cpp
)
add_library(neko::image ALIAS neko_image)

target_compile_features(neko_image PUBLIC cxx_std_20)
target_include_directories(neko_image PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_image PUBLIC neko::base ZLIB::ZLIB JPEG::JPEG)
apply_compiler_warnings(neko_image)
