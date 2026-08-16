#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "neko/base/status.h"
#include "neko/image/image.h"

namespace neko::pdf {

// One extracted page of text.
struct PdfPage {
  int index = 0;          // 0-based page number
  int width = 0;          // from /MediaBox (points), 0 if absent
  int height = 0;
  std::string text;       // best-effort extraction, "\n"-separated lines
};

struct PdfDocument {
  std::string title;      // from /Info /Title, empty if absent
  int page_count = 0;
  std::vector<PdfPage> pages;
};

// Extracts text from a PDF file.
//
// STATUS: PARTIALLY IMPLEMENTED.  Supports classic xref tables (including
// /Prev chains) and cross-reference streams (PDF 1.5, with /Index and
// /Prev chains), object streams (/ObjStm), indirect objects, streams,
// FlateDecode (with PNG/TIFF predictors), the pages tree (with inherited
// attributes such as /MediaBox) and the core text-showing operators
// (Tj/TJ/'/"/Td/TD/Tm/T*/BT/ET).  NOT IMPLEMENTED: encryption, forms,
// font/CMap-based Unicode mapping (text is mapped via UTF-16BE / ASCII /
// Latin-1 heuristics).
base::Result<PdfDocument> ExtractText(std::string_view data);

// True when the buffer begins with a "%PDF-" header.
bool IsPdf(std::string_view data);

// Renders one page of a PDF to an RGBA image (opaque white background,
// top-down rows).  |scale| zooms the page (1.0 = 72 dpi).
//
// STATUS: PARTIALLY IMPLEMENTED.  Renders vector graphics (rectangles,
// line/cubic-bezier paths with non-zero-winding and even-odd fills, strokes),
// the transform operators (cm, q/Q state stack) and text (BT/ET, Tf, Td/TD/
// Tm/T*, TL, Tj/TJ/'/", Tc/Tw).  Supports classic xref tables and xref
// streams, object streams, and inherited page attributes (/MediaBox).
// Glyphs are drawn through the engine's FreeType font stack; glyph widths
// come from the font dictionary's /Widths array when present, otherwise a
// 0.5-em approximation.  Text is decoded as Latin-1 (simple-font heuristic).
// NOT IMPLEMENTED: image XObjects and inline images, clipping paths,
// patterns/shadings, CMap/Type1-font-aware text, encryption.
base::Result<image::Image> RenderPage(std::string_view data, int page_index, float scale);

}  // namespace neko::pdf
