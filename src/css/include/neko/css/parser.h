#pragma once

#include "neko/css/stylesheet.h"

#include <string_view>

namespace neko::css {

// Parses a full stylesheet (rules + at-rules).  Malformed input is handled
// permissively: unrecognized constructs are skipped rather than failing the
// whole sheet.
StyleSheet ParseStyleSheet(std::string_view text);

// Parses a declaration block body ("color: red; margin: 0") — used for inline
// style attributes.
std::vector<Declaration> ParseDeclarationBlock(std::string_view text);

} // namespace neko::css
