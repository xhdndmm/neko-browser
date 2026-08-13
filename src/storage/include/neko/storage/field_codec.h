#pragma once

#include <string>
#include <string_view>

#include "neko/base/status.h"

namespace neko::storage {

// ---------------------------------------------------------------------------
// Field encoding for line-oriented storage files.
//
// Storage records are written as tab-separated fields.  Because titles,
// URLs and cookie values may contain tabs, newlines or other control bytes,
// each field is percent-encoded before it is written.  Encode/Decode always
// round-trip; Decode rejects malformed input with a Parse error.
// ---------------------------------------------------------------------------

// Encodes |value| so the result contains only the unreserved characters
// [A-Za-z0-9._~-]; every other byte becomes %XX (upper-case hex).
std::string EncodeField(std::string_view value);

// Decodes a field produced by EncodeField().  Returns a Parse error on
// malformed percent-escapes or a trailing '%'.
base::Result<std::string> DecodeField(std::string_view value);

}  // namespace neko::storage
