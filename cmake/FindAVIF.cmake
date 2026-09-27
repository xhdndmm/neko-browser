# FindAVIF.cmake — locate the system libavif and provide the AVIF::avif
# imported target (same pattern as FindWebP.cmake; Ubuntu 24.04 ships
# libavif-dev).
#
# The library found here must have been built with an AV1 decoder (dav1d/aom):
# libavif itself is only the container parser/codec API, so a decoder-less
# build parses AVIF files but fails to decode them at runtime.  vcpkg's libavif
# port has no default features — install 'libavif[dav1d]' (or [aom]).

find_path(AVIF_INCLUDE_DIR
  NAMES avif/avif.h
  DOC "Path to the libavif include directory"
)

find_library(AVIF_LIBRARY
  NAMES avif
  DOC "Path to the libavif library"
)

mark_as_advanced(AVIF_INCLUDE_DIR AVIF_LIBRARY)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(AVIF
  REQUIRED_VARS AVIF_LIBRARY AVIF_INCLUDE_DIR
)

if(AVIF_FOUND AND NOT TARGET AVIF::avif)
  add_library(AVIF::avif UNKNOWN IMPORTED)
  set_target_properties(AVIF::avif PROPERTIES
    IMPORTED_LOCATION "${AVIF_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${AVIF_INCLUDE_DIR}"
  )

  # A static libavif (e.g. the vcpkg *-windows-static-md triplet used by the
  # release build) is just the container/codec API: its AV1 decoder (dav1d)
  # and the colour-conversion helper libyuv (a base dependency of vcpkg's
  # libavif port) are separate archives with no link interface, so they have
  # to be added explicitly.  A dynamic libavif (import library or .so)
  # already carries these as shared-library dependencies.
  if(DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "static")
    find_library(AVIF_DAV1D_LIBRARY NAMES dav1d
      DOC "dav1d AV1 decoder (static libavif dependency)")
    find_library(AVIF_LIBYUV_LIBRARY NAMES yuv libyuv
      DOC "libyuv (static libavif dependency)")
    mark_as_advanced(AVIF_DAV1D_LIBRARY AVIF_LIBYUV_LIBRARY)

    set(_avif_static_deps)
    foreach(_avif_dep IN ITEMS "${AVIF_DAV1D_LIBRARY}" "${AVIF_LIBYUV_LIBRARY}")
      if(_avif_dep)
        list(APPEND _avif_static_deps "${_avif_dep}")
      endif()
    endforeach()
    if(_avif_static_deps)
      set_property(TARGET AVIF::avif APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "${_avif_static_deps}")
    endif()
  endif()
endif()
