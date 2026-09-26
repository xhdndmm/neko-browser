# =============================================================================
# FindFFmpeg - locates the FFmpeg libraries (libavformat, libavcodec,
# libavutil, libswscale) and exposes them as the imported target
# FFmpeg::FFmpeg.
#
# Usage:
#   find_package(FFmpeg REQUIRED)
#   target_link_libraries(foo PRIVATE FFmpeg::FFmpeg)
#
# Discovery order (whichever succeeds must yield an equivalent target):
#   1. pkg-config - Debian/Ubuntu (`libavformat-dev` and friends), Homebrew and
#      other Unix systems.  Preferred where available because the .pc files
#      also carry the transitive link requirements.
#   2. Include/library search - used when pkg-config is missing or does not
#      know FFmpeg.  This is the Windows CI path: vcpkg installs the headers
#      and import libraries, but no pkg-config program is on PATH, so the
#      module cannot depend on pkg-config alone (it used to, which broke every
#      Windows configure).
#
# On Debian/Ubuntu the development packages are:
#   libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
#
# Optional override:
#   FFMPEG_ROOT - install prefix to search first (the usual <Package>_ROOT
#   convention).  The Windows release build uses it: every other dependency
#   comes from a static vcpkg triplet, while FFmpeg itself stays dynamically
#   linked (LGPL relink obligation, ADR 0014) and is installed into a second,
#   dynamic triplet that is not on CMAKE_PREFIX_PATH.
#
# The search assumes a shared or import-library FFmpeg, i.e. FFMPEG_LIBRARIES
# alone is enough to link.  That is deliberate: FFmpeg is kept dynamically
# linked on every platform and the release pipeline bundles the runtime
# libraries (ADR 0019).  A static FFmpeg would additionally need its own
# system dependencies (ws2_32, secur32, bcrypt, crypt32, ... on Windows) and
# LGPL relink materials; add both when such a build is ever needed.
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

# Empty unless the caller points FFMPEG_ROOT at an install prefix.
set(_ffmpeg_root_hints)
if(FFMPEG_ROOT)
  list(APPEND _ffmpeg_root_hints "${FFMPEG_ROOT}")
endif()

if(NOT TARGET PkgConfig::FFMPEG)
  find_path(FFMPEG_INCLUDE_DIR
    NAMES libavformat/avformat.h libavcodec/avcodec.h
    HINTS ${_ffmpeg_root_hints}
    PATH_SUFFIXES include
    DOC "Path to the FFmpeg include directory"
  )
  find_library(FFMPEG_AVFORMAT_LIBRARY NAMES avformat libavformat
    HINTS ${_ffmpeg_root_hints} PATH_SUFFIXES lib DOC "libavformat")
  find_library(FFMPEG_AVCODEC_LIBRARY NAMES avcodec libavcodec
    HINTS ${_ffmpeg_root_hints} PATH_SUFFIXES lib DOC "libavcodec")
  find_library(FFMPEG_AVUTIL_LIBRARY NAMES avutil libavutil
    HINTS ${_ffmpeg_root_hints} PATH_SUFFIXES lib DOC "libavutil")
  find_library(FFMPEG_SWSCALE_LIBRARY NAMES swscale libswscale
    HINTS ${_ffmpeg_root_hints} PATH_SUFFIXES lib DOC "libswscale")
  mark_as_advanced(
    FFMPEG_INCLUDE_DIR
    FFMPEG_AVFORMAT_LIBRARY
    FFMPEG_AVCODEC_LIBRARY
    FFMPEG_AVUTIL_LIBRARY
    FFMPEG_SWSCALE_LIBRARY
  )

  if(FFMPEG_INCLUDE_DIR
     AND FFMPEG_AVFORMAT_LIBRARY
     AND FFMPEG_AVCODEC_LIBRARY
     AND FFMPEG_AVUTIL_LIBRARY
     AND FFMPEG_SWSCALE_LIBRARY)
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_INCLUDE_DIR}")
    set(FFMPEG_LIBRARIES
      "${FFMPEG_AVFORMAT_LIBRARY}"
      "${FFMPEG_AVCODEC_LIBRARY}"
      "${FFMPEG_AVUTIL_LIBRARY}"
      "${FFMPEG_SWSCALE_LIBRARY}"
    )
  endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg
  REQUIRED_VARS FFMPEG_LIBRARIES FFMPEG_INCLUDE_DIRS
  VERSION_VAR FFMPEG_VERSION
)

# One project-facing target, independent of how FFmpeg was found:
# neko_media links this and never the discovery mechanism.
if(FFmpeg_FOUND AND NOT TARGET FFmpeg::FFmpeg)
  add_library(FFmpeg::FFmpeg INTERFACE IMPORTED)
  if(TARGET PkgConfig::FFMPEG)
    target_link_libraries(FFmpeg::FFmpeg INTERFACE PkgConfig::FFMPEG)
  else()
    set_target_properties(FFmpeg::FFmpeg PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIRS}"
      INTERFACE_LINK_LIBRARIES "${FFMPEG_LIBRARIES}"
    )
  endif()
endif()
