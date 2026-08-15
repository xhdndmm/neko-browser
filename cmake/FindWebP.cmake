# FindWebP.cmake — locate the system libwebp and provide the WebP::webp
# imported target.
#
# CMake >= 3.29 ships a built-in FindWebP module, but some CI toolchains and
# developer machines ship older CMake (e.g. Ubuntu 24.04's 3.28).  This module
# keeps find_package(WebP REQUIRED) working uniformly; it is preferred over the
# built-in module via CMAKE_MODULE_PATH (see the top-level CMakeLists.txt).

find_path(WebP_INCLUDE_DIR
  NAMES webp/decode.h
  DOC "Path to the libwebp include directory"
)

find_library(WebP_LIBRARY
  NAMES webp
  DOC "Path to the libwebp library"
)

mark_as_advanced(WebP_INCLUDE_DIR WebP_LIBRARY)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(WebP
  REQUIRED_VARS WebP_LIBRARY WebP_INCLUDE_DIR
)

if(WebP_FOUND AND NOT TARGET WebP::webp)
  add_library(WebP::webp UNKNOWN IMPORTED)
  set_target_properties(WebP::webp PROPERTIES
    IMPORTED_LOCATION "${WebP_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${WebP_INCLUDE_DIR}"
  )
endif()
