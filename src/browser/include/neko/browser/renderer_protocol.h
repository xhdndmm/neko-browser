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

// ---------------------------------------------------------------------------
// Renderer session protocol (ADR 0016 M2)
//
// M1 spawned a child per load and exchanged a single LoadRequest/LoadResult
// pair.  M2 keeps a child alive for the lifetime of a site instance and
// exchanges a stream of operations and replies, so the GUI can render frames
// on demand and forward user input:
//
//   browser                                    renderer child
//      |-- kLoad {bytes, content_type, url, viewport} -->|
//      |<-- reply {ok, title, changed, content_height} --|
//      |-- kSnapshot {viewport, scroll_y} -------------->|
//      |<-- reply {frame: rgba, ...} --------------------|
//      |-- kClick/kHover/kWheel/kKey/kScroll/kPump ----->|
//      |<-- reply {changed, url, hover_link, ...} -------|
//      |-- kShutdown ------------------------------------|
//
// The browser process owns navigation and the network stack (cookies stay
// browser-side): the child receives document bytes, never fetches its own
// top-level documents, and answers every request with a bounded reply.  All
// decoding is bounds-checked; the other end of the pipe stays part of the
// threat model.
// The reply layout changed when find-in-page arrived (new trailing fields), so
// the version moved to 2: an old child and a new browser now fail the version
// check instead of misreading each other's replies.
inline constexpr std::uint8_t kRendererSessionProtocolVersion = 2;

enum class SessionOp : std::uint8_t
{
  kLoad = 1,       // document bytes + content type + final URL + viewport
  kClick = 2,      // pointer click at document coordinates
  kHover = 3,      // pointer moved to document coordinates
  kHoverClear = 4, // pointer left the viewport
  kWheel = 5,      // wheel delta (px)
  kKey = 6,        // keydown / keyup
  kScroll = 7,     // viewport scroll offset changed (GUI scroll bar)
  kPump = 8,       // advance page timers / animations one frame
  kSnapshot = 9,   // rasterize the viewport at the given scroll offset
  kShutdown = 10,  // browser is done with this session
  kSetZoom = 11,   // user-facing page zoom factor (Ctrl+=/Ctrl+-)
  kFind = 12,      // find-in-page query / step (Ctrl+F)
};

struct RendererSessionRequest
{
  SessionOp op = SessionOp::kPump;

  // kLoad
  std::string document;     // raw document bytes
  std::string content_type; // e.g. "text/html; charset=utf-8"
  std::string url;          // final document URL (base for relative refs)
  int viewport_width = 0;
  int viewport_height = 0;

  // kSnapshot
  float scroll_y = 0;

  // kSetZoom: page zoom factor; the child clamps it to its own bounds.
  float zoom = 1.0F;

  // kFind: the find-in-page query plus a step direction (0 = new query,
  // +1 = next match, -1 = previous).  The child keeps the match list for its
  // document, so stepping only needs the query and the direction.
  std::string find_query;
  int find_direction = 0;

  // kClick / kHover
  float x = 0;
  float y = 0;

  // kWheel
  double delta_y = 0;

  // kKey
  std::string key_type; // "keydown" | "keyup"
  std::string key;
  std::string code;
};

struct RendererSessionReply
{
  bool ok = false;
  std::string error;

  // kClick / kKey: the page's cancelable event was not canceled and the
  // default action ran (a click on a hyperlink, a form submission, a key
  // that mutated a form control).
  bool handled = false;
  // The child's content changed since the previous reply: the browser should
  // request a fresh frame.
  bool changed = false;

  std::string url;
  std::string title;
  float content_height = 0;
  float scroll_y = 0;                  // the child's current scroll offset
  std::uint64_t scroll_request_id = 0; // script-requested scroll latch
  float pending_scroll_y = 0;
  std::string hover_link; // hyperlink target under the hovered point ("" = none)

  // kFind: how many matches the query has, which one is current (0-based, -1
  // when there is none) and the current match's rectangle in device pixels
  // (document coordinates).
  int find_count = 0;
  int find_index = -1;
  float find_x = 0;
  float find_y = 0;
  float find_width = 0;
  float find_height = 0;

  // A navigation happened inside the child (a link click or a script).  The
  // browser re-runs it through its own navigation path so cookies and content
  // routing stay browser-side; the child never fetches top-level documents.
  std::string redirect_url;

  // Frame (kSnapshot replies only).
  bool has_frame = false;
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba;
};

base::Result<std::string> EncodeSessionRequest(const RendererSessionRequest& request);
base::Result<RendererSessionRequest> DecodeSessionRequest(std::string_view payload);

base::Result<std::string> EncodeSessionReply(const RendererSessionReply& reply);
base::Result<RendererSessionReply> DecodeSessionReply(std::string_view payload);

} // namespace neko::browser
