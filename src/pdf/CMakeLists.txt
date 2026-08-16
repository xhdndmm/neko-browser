# =============================================================================
# neko_pdf — minimal PDF text extraction (PARTIAL).
#
# Classic xref tables (+ /Prev chains), object model, streams with
# FlateDecode and PNG/TIFF predictors, pages tree, core text operators.
# NOT IMPLEMENTED: xref streams, object streams, rendering, CMap/Unicode
# mapping, encryption.  See include/neko/pdf/pdf.h.
# =============================================================================

add_library(neko_pdf
  src/parser.cpp
)
add_library(neko::pdf ALIAS neko_pdf)

target_compile_features(neko_pdf PUBLIC cxx_std_20)
target_include_directories(neko_pdf PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_pdf PUBLIC neko::base neko::graphics neko::image ZLIB::ZLIB)
apply_compiler_warnings(neko_pdf)
