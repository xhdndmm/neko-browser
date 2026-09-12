#pragma once

#include "neko/base/status.h"
#include "neko/browser/renderer_protocol.h"
#include "neko/ipc/subprocess.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neko::browser {

// A frame produced by a renderer session: viewport-sized RGBA8888 pixels.
struct RemoteFrame
{
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba;
};

// The status a renderer session reports after one operation (ADR 0016 M2).
struct RendererUpdate
{
  // kClick / kKey: the page's default action ran (the session consumed the
  // event).  Mirrors the in-process dispatch return values.
  bool handled = false;
  // Content changed since the previous update: the caller should request a
  // fresh frame.
  bool changed = false;

  std::string url;
  std::string title;
  float content_height = 0;
  float scroll_y = 0;
  // Latch for a script-requested scroll (window.scrollTo / element.scrollTop).
  std::uint64_t scroll_request_id = 0;
  float pending_scroll_y = 0;
  // Hyperlink target under the last hovered point ("" = none).  The GUI shows
  // a pointing hand from this without needing the DOM.
  std::string hover_link;

  // Find-in-page (kFind): total matches, the current one (0-based, -1 when
  // none) and its rectangle in device pixels (document coordinates).
  int find_count = 0;
  int find_index = -1;
  float find_x = 0;
  float find_y = 0;
  float find_width = 0;
  float find_height = 0;

  // A navigation happened inside the child; the browser re-runs it through its
  // own navigation path (cookies and content routing stay browser-side).
  std::string redirect_url;
};

// Browser-process side of a renderer session (ADR 0016 M2).
//
// Owns one renderer child process (the browser binary in --renderer-session
// mode) and speaks the session protocol over its stdio channel.  Every method
// blocks until the child replies, which matches the controller's synchronous
// worker-thread design; the child does the parsing, styling, layout, painting
// and page scripting in its own address space, so a crash there surfaces as an
// error here instead of taking the browser down.
//
// Threading: not thread-safe; the owning controller uses it from its worker
// thread only.
class RendererSession
{
public:
  // Spawns |executable| with --renderer-session.  |origin| names the site this
  // session serves (diagnostics only).
  static base::Result<std::shared_ptr<RendererSession>> Spawn(const std::string& executable,
                                                              std::string origin);
  ~RendererSession();

  RendererSession(const RendererSession&) = delete;
  RendererSession& operator=(const RendererSession&) = delete;

  // Loads a fetched document (HTML bytes + content type + final URL) and lays
  // it out at |viewport_width| x |viewport_height|.
  base::Result<RendererUpdate> Load(std::string_view bytes,
                                    std::string_view content_type,
                                    const std::string& url,
                                    int viewport_width,
                                    int viewport_height);

  base::Result<RendererUpdate> Click(float doc_x, float doc_y);
  base::Result<RendererUpdate> Hover(float doc_x, float doc_y);
  base::Result<RendererUpdate> HoverClear();
  base::Result<RendererUpdate> Wheel(double delta_y);
  base::Result<RendererUpdate>
  Key(std::string_view key_type, std::string_view key, std::string_view code);
  // Reports the GUI's scroll-bar offset (so window.scrollY reads it live).
  base::Result<RendererUpdate> ScrollTo(float y);
  // Applies the user-facing page zoom (Ctrl+=/Ctrl+-); the child clamps it.
  base::Result<RendererUpdate> SetZoom(float factor);
  // Find-in-page (Ctrl+F): |direction| 0 starts a new query, +1/-1 steps the
  // current match list (wrapping).  The child keeps the match list.
  base::Result<RendererUpdate> Find(std::string_view query, int direction);
  // Advances the page's timers and animated images by one frame.
  base::Result<RendererUpdate> Pump();
  // Rasterizes the viewport (|width| x |height|) scrolled to |scroll_y|.
  base::Result<RendererUpdate> Snapshot(int width, int height, float scroll_y, RemoteFrame* frame);

  // Sends kShutdown and reaps the child.  Safe to call more than once.
  void Shutdown();

  const std::string& origin() const
  {
    return origin_;
  }
  bool alive() const
  {
    return alive_;
  }
  // The child's process id (0 when not running).  For diagnostics and tests.
  long ProcessId() const
  {
    return child_.ProcessId();
  }

private:
  RendererSession(ipc::Subprocess child, std::string origin);

  base::Result<RendererSessionReply> RoundTrip(const RendererSessionRequest& request);
  static base::Result<RendererUpdate> Convert(const RendererSessionReply& reply,
                                              RemoteFrame* frame);

  ipc::Subprocess child_;
  std::string origin_;
  bool alive_ = false;
};

} // namespace neko::browser
