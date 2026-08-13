#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "neko/base/status.h"

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
// /Prev chains), indirect objects, streams, FlateDecode (with PNG/TIFF
// predictors), the pages tree and the core text-showing operators
// (Tj/TJ/'/"/Td/TD/Tm/T*/BT/ET).  NOT IMPLEMENTED: xref streams, object
// streams, rendering, font/CMap-based Unicode mapping (text is mapped via
// UTF-16BE / ASCII / Latin-1 heuristics), encryption, forms.
base::Result<PdfDocument> ExtractText(std::string_view data);

// True when the buffer begins with a "%PDF-" header.
bool IsPdf(std::string_view data);

}  // namespace neko::pdf
