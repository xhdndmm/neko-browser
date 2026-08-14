#pragma once

#include "neko/base/status.h"

#include <string>
#include <string_view>

namespace neko::network {

// ---------------------------------------------------------------------------
// HTTP content-coding decompression (RFC 7231 §3.1.2).
//
// zlib is used for the actual decompression; it is wrapped behind these
// project-owned functions so callers never touch zlib types directly.
// Decompression is bounded (see compression.cpp) so untrusted network
// content cannot exhaust memory.
// ---------------------------------------------------------------------------

// Decompresses a gzip stream (RFC 1952).  Returns a Parse error when |data|
// is not a valid gzip stream or the output exceeds the bound.
base::Result<std::string> InflateGzip(std::string_view data);

// Decompresses a "deflate" stream.  RFC 7230 §4.2.2 leaves the wire format
// ambiguous (zlib wrapper vs. raw deflate), so both forms are accepted: a
// zlib (RFC 1950) stream is tried first, then a raw deflate stream.
base::Result<std::string> InflateDeflate(std::string_view data);

// Applies a Content-Encoding header value to |body|.  Encodings are listed
// in the order they were applied, so they are decoded in reverse order
// (RFC 7231 §3.1.2.1: "Content-Encoding: gzip, deflate" means the body was
// deflated first, then gzipped).  "identity" is a no-op; any other coding
// yields a Parse error rather than returning corrupted content.
base::Result<std::string> DecodeContentEncodings(std::string_view content_encoding,
                                                 std::string_view body);

} // namespace neko::network
