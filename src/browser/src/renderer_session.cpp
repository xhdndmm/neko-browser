#include "neko/browser/renderer_session.h"

#include "neko/base/logging.h"

#include <utility>

namespace neko::browser {

RendererSession::RendererSession(ipc::Subprocess child, std::string origin)
    : child_(std::move(child)), origin_(std::move(origin)), alive_(true)
{}

RendererSession::~RendererSession()
{
  Shutdown();
}

base::Result<std::shared_ptr<RendererSession>> RendererSession::Spawn(const std::string& executable,
                                                                      std::string origin)
{
  if (executable.empty()) {
    return base::Err(base::Error::InvalidArgument("no renderer executable"));
  }
  std::vector<std::string> argv = {executable, "--renderer-session", "--log-level", "warning"};
  auto spawned = ipc::Subprocess::Spawn(argv);
  if (!spawned.has_value()) {
    return base::Err(spawned.error());
  }
  // The constructor is private (sessions are created through Spawn): use the
  // explicit new-expression instead of std::make_shared.
  return std::shared_ptr<RendererSession>(
      new RendererSession(std::move(spawned.value()), std::move(origin)));
}

void RendererSession::Shutdown()
{
  if (!child_.channel().open()) {
    alive_ = false;
    return;
  }
  alive_ = false;
  RendererSessionRequest request;
  request.op = SessionOp::kShutdown;
  const auto encoded = EncodeSessionRequest(request);
  // Best-effort cooperative shutdown: the child replies and exits, so the
  // reap below returns promptly.  If even the send fails the child is gone
  // (or unreachable) and Terminate reaps it.
  if (encoded.has_value() && child_.channel().Send(encoded.value())) {
    (void)child_.channel().Receive();
  } else {
    child_.Terminate();
  }
  (void)child_.Wait();
  child_.channel().Close();
}

base::Result<RendererSessionReply> RendererSession::RoundTrip(const RendererSessionRequest& request)
{
  if (!alive_) {
    return base::Err(base::Error::Io("renderer session is not running"));
  }
  const auto encoded = EncodeSessionRequest(request);
  if (!encoded.has_value()) {
    return base::Err(encoded.error());
  }
  if (!child_.channel().Send(encoded.value())) {
    alive_ = false;
    return base::Err(base::Error::Io("renderer session closed the connection"));
  }
  auto frame = child_.channel().Receive();
  if (!frame.has_value()) {
    // A closed pipe or a malformed frame means the session is no longer
    // usable; the caller tears it down and can spawn a fresh one.
    alive_ = false;
    return base::Err(frame.error());
  }
  auto reply = DecodeSessionReply(frame.value());
  if (!reply.has_value()) {
    alive_ = false;
    return base::Err(reply.error());
  }
  if (!reply.value().ok) {
    const std::string& message = reply.value().error;
    return base::Err(base::Error::Io(message.empty() ? "renderer session error" : message));
  }
  return reply.value();
}

base::Result<RendererUpdate> RendererSession::Convert(const RendererSessionReply& reply,
                                                      RemoteFrame* frame)
{
  RendererUpdate update;
  update.handled = reply.handled;
  update.changed = reply.changed;
  update.url = reply.url;
  update.title = reply.title;
  update.content_height = reply.content_height;
  update.scroll_y = reply.scroll_y;
  update.scroll_request_id = reply.scroll_request_id;
  update.pending_scroll_y = reply.pending_scroll_y;
  update.hover_link = reply.hover_link;
  update.redirect_url = reply.redirect_url;
  if (frame != nullptr) {
    frame->width = 0;
    frame->height = 0;
    frame->rgba.clear();
    if (reply.has_frame) {
      frame->width = reply.width;
      frame->height = reply.height;
      frame->rgba = reply.rgba;
    }
  }
  return update;
}

base::Result<RendererUpdate> RendererSession::Load(std::string_view bytes,
                                                   std::string_view content_type,
                                                   const std::string& url,
                                                   int viewport_width,
                                                   int viewport_height)
{
  RendererSessionRequest request;
  request.op = SessionOp::kLoad;
  request.document.assign(bytes);
  request.content_type.assign(content_type);
  request.url = url;
  request.viewport_width = viewport_width;
  request.viewport_height = viewport_height;
  auto reply = RoundTrip(request);
  if (!reply.has_value()) {
    return base::Err(reply.error());
  }
  return Convert(reply.value(), nullptr);
}

base::Result<RendererUpdate> RendererSession::Click(float doc_x, float doc_y)
{
  RendererSessionRequest request;
  request.op = SessionOp::kClick;
  request.x = doc_x;
  request.y = doc_y;
  auto reply = RoundTrip(request);
  if (!reply.has_value()) {
    return base::Err(reply.error());
  }
  return Convert(reply.value(), nullptr);
}

base::Result<RendererUpdate> RendererSession::Hover(float doc_x, float doc_y)
{
  RendererSessionRequest request;
  request.op = SessionOp::kHover;
  request.x = doc_x;
  request.y = doc_y;
  auto reply = RoundTrip(request);
  if (!reply.has_value()) {
    return base::Err(reply.error());
  }
  return Convert(reply.value(), nullptr);
}

base::Result<RendererUpdate> RendererSession::HoverClear()
{
  RendererSessionRequest request;
  request.op = SessionOp::kHoverClear;
  auto reply = RoundTrip(request);
  if (!reply.has_value()) {
    return base::Err(reply.error());
  }
  return Convert(reply.value(), nullptr);
}

base::Result<RendererUpdate> RendererSession::Wheel(double delta_y)
{
  RendererSessionRequest request;
  request.op = SessionOp::kWheel;
  request.delta_y = delta_y;
  auto reply = RoundTrip(request);
  if (!reply.has_value()) {
    return base::Err(reply.error());
  }
  return Convert(reply.value(), nullptr);
}

base::Result<RendererUpdate>
RendererSession::Key(std::string_view key_type, std::string_view key, std::string_view code)
{
  RendererSessionRequest request;
  request.op = SessionOp::kKey;
  request.key_type.assign(key_type);
  request.key.assign(key);
  request.code.assign(code);
  auto reply = RoundTrip(request);
  if (!reply.has_value()) {
    return base::Err(reply.error());
  }
  return Convert(reply.value(), nullptr);
}

base::Result<RendererUpdate> RendererSession::ScrollTo(float y)
{
  RendererSessionRequest request;
  request.op = SessionOp::kScroll;
  request.scroll_y = y;
  auto reply = RoundTrip(request);
  if (!reply.has_value()) {
    return base::Err(reply.error());
  }
  return Convert(reply.value(), nullptr);
}

base::Result<RendererUpdate> RendererSession::Pump()
{
  RendererSessionRequest request;
  request.op = SessionOp::kPump;
  auto reply = RoundTrip(request);
  if (!reply.has_value()) {
    return base::Err(reply.error());
  }
  return Convert(reply.value(), nullptr);
}

base::Result<RendererUpdate> RendererSession::SetZoom(float factor)
{
  RendererSessionRequest request;
  request.op = SessionOp::kSetZoom;
  request.zoom = factor;
  auto reply = RoundTrip(request);
  if (!reply.has_value()) {
    return base::Err(reply.error());
  }
  return Convert(reply.value(), nullptr);
}

base::Result<RendererUpdate>
RendererSession::Snapshot(int width, int height, float scroll_y, RemoteFrame* frame)
{
  RendererSessionRequest request;
  request.op = SessionOp::kSnapshot;
  request.viewport_width = width;
  request.viewport_height = height;
  request.scroll_y = scroll_y;
  auto reply = RoundTrip(request);
  if (!reply.has_value()) {
    return base::Err(reply.error());
  }
  return Convert(reply.value(), frame);
}

} // namespace neko::browser
