#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace neko::base::encoding {

// Supported character encodings, mirroring the WHATWG Encoding Standard
// (https://encoding.spec.whatwg.org/).  All decoders follow the standard's
// algorithms, including error handling ("replacement" error mode: invalid
// sequences produce U+FFFD).  GBK and gb18030 share the gb18030 decoder, as
// the standard defines.
enum class Charset
{
  kUtf8,
  // Legacy single-byte encodings.
  kIbm866,
  kIso88592,
  kIso88593,
  kIso88594,
  kIso88595,
  kIso88596,
  kIso88597,
  kIso88598,
  kIso88598I,
  kIso885910,
  kIso885913,
  kIso885914,
  kIso885915,
  kIso885916,
  kKoi8R,
  kKoi8U,
  kMacintosh,
  kWindows874,
  kWindows1250,
  kWindows1251,
  kWindows1252,
  kWindows1253,
  kWindows1254,
  kWindows1255,
  kWindows1256,
  kWindows1257,
  kWindows1258,
  kXMacCyrillic,
  // Legacy multi-byte encodings.
  kGb18030, // also used for the GBK / gb2312 labels
  kBig5,
  kEucJp,
  kIso2022Jp,
  kShiftJis,
  kEucKr,
  // Miscellaneous.
  kReplacement,
  kUtf16Be,
  kUtf16Le,
  kXUserDefined,
  kUnknown,
};

// Canonical WHATWG name of |charset| (e.g. "UTF-8", "windows-1252",
// "gb18030", "Shift_JIS").  kUnknown -> "unknown".
std::string_view CharsetName(Charset charset);

// Resolves an encoding label per WHATWG "get an encoding": the label is
// trimmed, ASCII-case-insensitively matched against the standard's label
// table.  Returns nullopt for unknown labels and for the replacement
// encoding (callers that need it can use CharsetFromLabelOrReplacement).
std::optional<Charset> CharsetFromLabel(std::string_view label);

// Like CharsetFromLabel but maps the "replacement" labels to
// Charset::kReplacement instead of nullopt.
std::optional<Charset> CharsetFromLabelOrReplacement(std::string_view label);

// Extracts the charset parameter of an HTTP Content-Type header value
// (e.g. "text/html; charset=gb2312" -> kGb18030).  Returns nullopt when the
// header has no charset parameter or the value is not a known label.
std::optional<Charset> CharsetFromHttpHeader(std::string_view content_type);

// Decodes |bytes| (raw, arbitrary encoding) into a UTF-8 std::string,
// following the WHATWG decoder for |charset| with the "replacement" error
// mode (invalid input becomes U+FFFD, U+EF BF BD in UTF-8).  A UTF-8 BOM at
// the start of |bytes| overrides |charset|, matching the standard's decode
// algorithm.  kUnknown behaves as UTF-8.
std::string DecodeToUtf8(std::string_view bytes, Charset charset);

// Sniffs a leading byte-order mark.  Returns nullopt when there is no BOM.
std::optional<Charset> SniffBom(std::string_view bytes);

// HTML charset detection (WHATWG HTML, "prescan a byte stream to determine
// its encoding"): checks UTF-16 signatures and scans the first 1024 bytes for
// <meta charset> / <meta http-equiv=Content-Type content="...; charset=...">.
// |http_hint| (from the Content-Type header, if any) is used as the fallback
// before the standard's windows-1252 default.  Returns kWindows1252 as the
// final fallback when nothing is found.
Charset DetectHtmlCharset(std::string_view bytes, std::optional<Charset> http_hint = std::nullopt);

} // namespace neko::base::encoding
