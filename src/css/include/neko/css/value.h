#pragma once

#include "neko/css/color.h"

#include <optional>
#include <string>
#include <string_view>

namespace neko::css {

// A parsed CSS value (Phase 4 scope).
struct CssValue
{
  enum class Type
  {
    kKeyword,
    kLength,
    kNumber,
    kColor,
    kString,
    kUnparsed
  };

  Type type = Type::kUnparsed;
  std::string text;        // keyword text / raw unparsed text
  float number = 0;        // kNumber
  float value = 0;         // kLength numeric part
  bool is_percent = false; // kLength with % unit
  std::string unit;        // "px" / "em" / "rem" / "%" for kLength
  std::optional<Color> color;
};

// Parses a declaration value into a typed value.  Colors, lengths (px/em/rem/%)
// and numbers are recognized; anything else becomes a keyword or unparsed.
CssValue ParseCssValue(std::string_view text);

// True when |keyword| equals |text| case-insensitively.
bool IsKeyword(const CssValue& value, std::string_view keyword);

} // namespace neko::css
