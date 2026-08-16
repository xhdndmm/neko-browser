#pragma once

#include "neko/base/status.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace neko::browser {

// Versioned binary wire format between the browser process and a renderer
// child (ADR 0016 M1).  The IPC layer guarantees whole framed payloads up to
// 64 MiB; these helpers encode/decode the payload itself.  All decoding is
// bounds-checked — the other end of the pipe is part of the threat model.
//
// Payload layout (little-endian):
//   LoadRequest:  u8 version(1) | u8 op(1 = load) | u32 url_len | url |
//                 u32 viewport_width | u32 viewport_height
//   LoadResult:   u8 version(1) | u8 status(0 = ok, 1 = error) |
//                 u32 width | u32 height | u64 rgba_len | rgba |
//                 u32 dom_len | dom | u32 title_len | title |
//                 u32 error_len | error
inline constexpr std::uint8_t kRendererProtocolVersion = 1;
inline constexpr std::uint8_t kRendererOpLoad = 1;

struct RendererLoadRequest
{
  std::string url;
  int viewport_width = 0;
  int viewport_height = 0;
};

struct RendererLoadResult
{
  bool ok = false;
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba; // RGBA8888, width*height*4 bytes when ok
  std::string dom;
  std::string title;
  std::string error; // non-empty when !ok
};

base::Result<std::string> EncodeLoadRequest(const RendererLoadRequest& request);
base::Result<RendererLoadRequest> DecodeLoadRequest(std::string_view payload);

base::Result<std::string> EncodeLoadResult(const RendererLoadResult& result);
base::Result<RendererLoadResult> DecodeLoadResult(std::string_view payload);

} // namespace neko::browser
