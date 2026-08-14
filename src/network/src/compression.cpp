// HTTP content-coding decompression (gzip / deflate) behind zlib.

#include "neko/network/compression.h"

#include "neko/base/status.h"
#include "neko/base/string_util.h"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <zlib.h>

namespace neko::network {
namespace {

// Upper bound on decompressed output.  gzip/deflate streams have no intrinsic
// size limit, so a bounded decompressor is required: untrusted network content
// must not be able to exhaust memory (zip-bomb).  64 MiB covers any realistic
// single HTTP body while staying a safe bound.
constexpr std::size_t kMaxDecompressedSize = 64u * 1024u * 1024u;

// Runs zlib inflate with the given |window_bits| (15+16 for gzip, 15 for a
// zlib stream, -15 for raw deflate).  Returns the decompressed bytes or a
// Parse error.  |out| is cleared and reused to avoid repeated reallocation.
base::Result<std::string> Inflate(std::string_view data, int window_bits, std::string& out)
{
  out.clear();
  if (data.empty()) {
    return base::Err(base::Error::Parse("empty compressed stream"));
  }
  z_stream strm{};
  if (inflateInit2(&strm, window_bits) != Z_OK) {
    return base::Err(base::Error::Parse("decompressor init failed"));
  }
  strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  strm.avail_in = static_cast<uInt>(data.size());
  if (strm.avail_in != data.size()) {
    inflateEnd(&strm);
    return base::Err(base::Error::Parse("compressed stream too large"));
  }

  std::string buffer(16384, '\0');
  int ret = Z_OK;
  for (;;) {
    strm.next_out = reinterpret_cast<Bytef*>(buffer.data());
    strm.avail_out = static_cast<uInt>(buffer.size());
    ret = inflate(&strm, Z_NO_FLUSH);
    const std::size_t produced = buffer.size() - strm.avail_out;
    if (out.size() + produced > kMaxDecompressedSize) {
      inflateEnd(&strm);
      return base::Err(base::Error::Parse("decompressed output too large"));
    }
    out.append(buffer.data(), produced);
    if (ret == Z_STREAM_END) {
      break;
    }
    if (ret != Z_OK && ret != Z_BUF_ERROR) {
      inflateEnd(&strm);
      return base::Err(base::Error::Parse("invalid deflate stream"));
    }
    // No input left and nothing produced: the stream is truncated or the
    // remaining bytes are not a valid deflate stream.
    if (strm.avail_in == 0 && produced == 0) {
      inflateEnd(&strm);
      return base::Err(base::Error::Parse("truncated deflate stream"));
    }
  }
  inflateEnd(&strm);
  return out;
}

// Splits a Content-Encoding value into trimmed, non-empty codings.
std::vector<std::string_view> SplitCodings(std::string_view content_encoding)
{
  std::vector<std::string_view> codings;
  std::size_t pos = 0;
  for (;;) {
    const std::size_t comma = content_encoding.find(',', pos);
    const std::size_t end = comma == std::string_view::npos ? content_encoding.size() : comma;
    const std::string_view part = base::Trim(content_encoding.substr(pos, end - pos));
    if (!part.empty()) {
      codings.push_back(part);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    pos = comma + 1;
  }
  return codings;
}

} // namespace

base::Result<std::string> InflateGzip(std::string_view data)
{
  std::string out;
  return Inflate(data, 15 + 16, out); // windowBits + 16 selects the gzip wrapper
}

base::Result<std::string> InflateDeflate(std::string_view data)
{
  // RFC 7230 §4.2.2: "deflate" is ambiguous — some servers send a zlib
  // (RFC 1950) stream, others a raw deflate stream.  Try zlib first, then
  // raw deflate (negative window bits = raw).
  std::string out;
  const base::Result<std::string> zlib_wrapped = Inflate(data, 15, out);
  if (zlib_wrapped) {
    return zlib_wrapped;
  }
  return Inflate(data, -15, out);
}

base::Result<std::string> DecodeContentEncodings(std::string_view content_encoding,
                                                 std::string_view body)
{
  const std::vector<std::string_view> codings = SplitCodings(content_encoding);
  if (codings.empty()) {
    return std::string(body);
  }
  std::string current(body);
  for (auto it = codings.rbegin(); it != codings.rend(); ++it) {
    if (base::AsciiEqualsIgnoreCase(*it, "identity")) {
      continue;
    }
    if (base::AsciiEqualsIgnoreCase(*it, "gzip") || base::AsciiEqualsIgnoreCase(*it, "x-gzip")) {
      const base::Result<std::string> decoded = InflateGzip(current);
      if (!decoded) {
        return base::Err(decoded.error());
      }
      current = std::move(decoded.value());
      continue;
    }
    if (base::AsciiEqualsIgnoreCase(*it, "deflate")) {
      const base::Result<std::string> decoded = InflateDeflate(current);
      if (!decoded) {
        return base::Err(decoded.error());
      }
      current = std::move(decoded.value());
      continue;
    }
    return base::Err(base::Error::Parse("unsupported content-encoding '" + std::string(*it) + "'"));
  }
  return current;
}

} // namespace neko::network
