#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace neko::base {

// ---------------------------------------------------------------------------
// String utilities (ASCII scope)
// ---------------------------------------------------------------------------
// Phase 0 ships ASCII-focused helpers with well-defined, portable semantics.
// Unicode-aware handling (case folding, normalization, graphemes) arrives with
// the text/ICU work in later phases; do not extend these helpers with
// locale-dependent behavior.

// Case-insensitive ASCII comparison.
bool AsciiEqualsIgnoreCase(std::string_view lhs, std::string_view rhs);

bool AsciiStartsWith(std::string_view text, std::string_view prefix);
bool AsciiEndsWith(std::string_view text, std::string_view suffix);

bool Contains(std::string_view text, std::string_view needle);

// Trims ASCII whitespace from both ends.
std::string_view Trim(std::string_view text);
std::string_view TrimLeft(std::string_view text);
std::string_view TrimRight(std::string_view text);

// ASCII case conversion (never locale-dependent).
std::string ToLower(std::string_view text);
std::string ToUpper(std::string_view text);

// Splits on every occurrence of the delimiter.  Empty segments are preserved.
std::vector<std::string_view> Split(std::string_view text, char delimiter);
std::vector<std::string_view> Split(std::string_view text, std::string_view delimiter);

// Joins parts with the given separator.
std::string Join(const std::vector<std::string_view>& parts, std::string_view separator);

// Replaces every non-overlapping occurrence of |from| with |to|.
std::string ReplaceAll(std::string_view text, std::string_view from, std::string_view to);

} // namespace neko::base
