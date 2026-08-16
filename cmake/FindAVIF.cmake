# FindAVIF.cmake — locate the system libavif and provide the AVIF::avif
# imported target (same pattern as FindWebP.cmake; Ubuntu 24.04 ships
# libavif-dev).

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
endif()
