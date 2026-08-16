# =============================================================================
# FindFFmpeg - locates the FFmpeg libraries (libavformat, libavcodec,
# libavutil, libswscale) and exposes them as the imported target
# PkgConfig::FFMPEG.
#
# Usage:
#   find_package(FFmpeg REQUIRED)
#   target_link_libraries(foo PRIVATE PkgConfig::FFMPEG)
#
# On Debian/Ubuntu the development packages are:
#   libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
# =============================================================================

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswscale
  )
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg
  REQUIRED_VARS FFMPEG_FOUND
  VERSION_VAR FFMPEG_VERSION
)
