# =============================================================================
# neko_media — audio / video (media subsystem).
#
# Audio: WAV/RIFF decoding implemented from scratch (PCM + IEEE float,
# 8/16/24/32-bit).
# Video: FFmpeg (libavformat/libavcodec/libswscale) demuxing + decoding
# wrapped behind neko::media::MediaSource / DecodeVideo (ADR 0014); FFmpeg
# headers stay inside video.cpp.
# =============================================================================

find_package(FFmpeg REQUIRED)

add_library(neko_media
  src/audio.cpp
  src/video.cpp
)
add_library(neko::media ALIAS neko_media)

target_compile_features(neko_media PUBLIC cxx_std_20)
target_include_directories(neko_media PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_media PUBLIC neko::base neko::image PRIVATE PkgConfig::FFMPEG)
apply_compiler_warnings(neko_media)
