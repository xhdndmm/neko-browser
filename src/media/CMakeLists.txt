# =============================================================================
# neko_media — audio / video (media subsystem, in progress).
#
# Audio: WAV/RIFF decoding implemented from scratch (PCM + IEEE float,
# 8/16/24/32-bit).
# Video: NOT IMPLEMENTED — MediaSource is the reserved architecture seam and
# returns explicit errors (see include/neko/media/media_source.h).
# =============================================================================

add_library(neko_media
  src/audio.cpp
)
add_library(neko::media ALIAS neko_media)

target_compile_features(neko_media PUBLIC cxx_std_20)
target_include_directories(neko_media PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_media PUBLIC neko::base)
apply_compiler_warnings(neko_media)
