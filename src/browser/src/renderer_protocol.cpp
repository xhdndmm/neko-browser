#include "neko/browser/renderer_protocol.h"

#include <algorithm>
#include <bit>

namespace neko::browser {

namespace {

// A bounds-checked cursor over a wire payload.  Every read validates its
// range; malformed input surfaces as InvalidArgument instead of UB.
class Reader
{
public:
  explicit Reader(std::string_view payload) : data_(payload.data()), size_(payload.size()) {}

  bool Ok() const
  {
    return ok_;
  }
  std::size_t Remaining() const
  {
    return pos_ <= size_ ? size_ - pos_ : 0;
  }

  base::Error Take(std::size_t n, const uint8_t** out)
  {
    if (!ok_ || pos_ > size_ || size_ - pos_ < n) {
      ok_ = false;
      return base::Error::InvalidArgument("truncated payload");
    }
    *out = reinterpret_cast<const uint8_t*>(data_ + pos_);
    pos_ += n;
    return base::Error();
  }

  base::Error U8(std::uint8_t* out)
  {
    const uint8_t* p = nullptr;
    if (auto e = Take(1, &p); !e.ok()) {
      return e;
    }
    *out = p[0];
    return base::Error();
  }

  base::Error U32(std::uint32_t* out)
  {
    const uint8_t* p = nullptr;
    if (auto e = Take(4, &p); !e.ok()) {
      return e;
    }
    *out = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    return base::Error();
  }

  base::Error U64(std::uint64_t* out)
  {
    const uint8_t* p = nullptr;
    if (auto e = Take(8, &p); !e.ok()) {
      return e;
    }
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
      value = (value << 8) | p[i];
    }
    *out = value;
    return base::Error();
  }

  base::Error String(std::uint32_t max_len, std::string* out)
  {
    std::uint32_t len = 0;
    if (auto e = U32(&len); !e.ok()) {
      return e;
    }
    if (len > max_len) {
      ok_ = false;
      return base::Error::InvalidArgument("field exceeds its size cap");
    }
    const uint8_t* p = nullptr;
    if (auto e = Take(len, &p); !e.ok()) {
      return e;
    }
    out->assign(reinterpret_cast<const char*>(p), len);
    return base::Error();
  }

  base::Error F32(float* out)
  {
    std::uint32_t bits = 0;
    if (auto e = U32(&bits); !e.ok()) {
      return e;
    }
    *out = std::bit_cast<float>(bits);
    return base::Error();
  }

  base::Error F64(double* out)
  {
    std::uint64_t bits = 0;
    if (auto e = U64(&bits); !e.ok()) {
      return e;
    }
    *out = std::bit_cast<double>(bits);
    return base::Error();
  }

private:
  const char* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

// Field size caps (independent of the 64 MiB frame cap; keep a hostile peer
// from asking for absurd allocations inside a well-formed frame).
inline constexpr std::uint32_t kMaxUrlBytes = 8u * 1024u * 1024u;
inline constexpr std::uint32_t kMaxTextFieldBytes = 64u * 1024u * 1024u;
inline constexpr std::uint64_t kMaxRgbaBytes = 512u * 1024u * 1024u;
// Session protocol caps: documents and short metadata fields.
inline constexpr std::uint32_t kMaxDocumentBytes = 32u * 1024u * 1024u;
inline constexpr std::uint32_t kMaxSessionTextBytes = 1024u * 1024u;

void PutU8(std::string& out, std::uint8_t v)
{
  out.push_back(static_cast<char>(v));
}

void PutU32(std::string& out, std::uint32_t v)
{
  out.push_back(static_cast<char>(v & 0xff));
  out.push_back(static_cast<char>((v >> 8) & 0xff));
  out.push_back(static_cast<char>((v >> 16) & 0xff));
  out.push_back(static_cast<char>((v >> 24) & 0xff));
}

void PutU64(std::string& out, std::uint64_t v)
{
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
  }
}

void PutString(std::string& out, std::string_view text)
{
  PutU32(out, static_cast<std::uint32_t>(text.size()));
  out.append(text.data(), text.size());
}

void PutF32(std::string& out, float v)
{
  PutU32(out, std::bit_cast<std::uint32_t>(v));
}

void PutF64(std::string& out, double v)
{
  PutU64(out, std::bit_cast<std::uint64_t>(v));
}

} // namespace

base::Result<std::string> EncodeLoadRequest(const RendererLoadRequest& request)
{
  if (request.url.size() > kMaxUrlBytes) {
    return base::Err(base::Error::InvalidArgument("request URL exceeds the cap"));
  }
  std::string out;
  out.reserve(request.url.size() + 16);
  PutU8(out, kRendererProtocolVersion);
  PutU8(out, kRendererOpLoad);
  PutString(out, request.url);
  PutU32(out, static_cast<std::uint32_t>(request.viewport_width));
  PutU32(out, static_cast<std::uint32_t>(request.viewport_height));
  return out;
}

base::Result<RendererLoadRequest> DecodeLoadRequest(std::string_view payload)
{
  Reader reader(payload);
  RendererLoadRequest request;
  std::uint8_t version = 0;
  std::uint8_t op = 0;
  if (auto e = reader.U8(&version); !e.ok()) {
    return base::Err(e);
  }
  if (version != kRendererProtocolVersion) {
    return base::Err(base::Error::InvalidArgument("unsupported protocol version"));
  }
  if (auto e = reader.U8(&op); !e.ok()) {
    return base::Err(e);
  }
  if (op != kRendererOpLoad) {
    return base::Err(base::Error::InvalidArgument("unknown operation"));
  }
  if (auto e = reader.String(kMaxUrlBytes, &request.url); !e.ok()) {
    return base::Err(e);
  }
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  if (auto e = reader.U32(&width); !e.ok()) {
    return base::Err(e);
  }
  if (auto e = reader.U32(&height); !e.ok()) {
    return base::Err(e);
  }
  if (reader.Remaining() != 0) {
    return base::Err(base::Error::InvalidArgument("trailing bytes in request"));
  }
  request.viewport_width = static_cast<int>(width);
  request.viewport_height = static_cast<int>(height);
  return request;
}

base::Result<std::string> EncodeLoadResult(const RendererLoadResult& result)
{
  if (result.rgba.size() > kMaxRgbaBytes || result.dom.size() > kMaxTextFieldBytes ||
      result.title.size() > kMaxTextFieldBytes || result.error.size() > kMaxTextFieldBytes) {
    return base::Err(base::Error::InvalidArgument("result field exceeds its size cap"));
  }
  std::string out;
  out.reserve(result.rgba.size() + result.dom.size() + 64);
  PutU8(out, kRendererProtocolVersion);
  PutU8(out, result.ok ? 0 : 1);
  PutU32(out, static_cast<std::uint32_t>(result.width));
  PutU32(out, static_cast<std::uint32_t>(result.height));
  PutU64(out, static_cast<std::uint64_t>(result.rgba.size()));
  out.append(reinterpret_cast<const char*>(result.rgba.data()), result.rgba.size());
  PutString(out, result.dom);
  PutString(out, result.title);
  PutString(out, result.error);
  return out;
}

base::Result<RendererLoadResult> DecodeLoadResult(std::string_view payload)
{
  Reader reader(payload);
  RendererLoadResult result;
  std::uint8_t version = 0;
  std::uint8_t status = 0;
  if (auto e = reader.U8(&version); !e.ok()) {
    return base::Err(e);
  }
  if (version != kRendererProtocolVersion) {
    return base::Err(base::Error::InvalidArgument("unsupported protocol version"));
  }
  if (auto e = reader.U8(&status); !e.ok()) {
    return base::Err(e);
  }
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint64_t rgba_len = 0;
  if (auto e = reader.U32(&width); !e.ok()) {
    return base::Err(e);
  }
  if (auto e = reader.U32(&height); !e.ok()) {
    return base::Err(e);
  }
  if (auto e = reader.U64(&rgba_len); !e.ok()) {
    return base::Err(e);
  }
  if (rgba_len > kMaxRgbaBytes) {
    return base::Err(base::Error::InvalidArgument("bitmap exceeds its size cap"));
  }
  const uint8_t* rgba = nullptr;
  if (auto e = reader.Take(static_cast<std::size_t>(rgba_len), &rgba); !e.ok()) {
    return base::Err(e);
  }
  result.width = static_cast<int>(width);
  result.height = static_cast<int>(height);
  result.rgba.assign(rgba, rgba + rgba_len);
  if (auto e = reader.String(kMaxTextFieldBytes, &result.dom); !e.ok()) {
    return base::Err(e);
  }
  if (auto e = reader.String(kMaxTextFieldBytes, &result.title); !e.ok()) {
    return base::Err(e);
  }
  if (auto e = reader.String(kMaxTextFieldBytes, &result.error); !e.ok()) {
    return base::Err(e);
  }
  if (reader.Remaining() != 0) {
    return base::Err(base::Error::InvalidArgument("trailing bytes in result"));
  }
  result.ok = status == 0;
  return result;
}

// ---------------------------------------------------------------------------
// Renderer session protocol (ADR 0016 M2)
// ---------------------------------------------------------------------------

namespace {

inline constexpr std::uint8_t kReplyFlagHandled = 1U << 0;
inline constexpr std::uint8_t kReplyFlagChanged = 1U << 1;
inline constexpr std::uint8_t kReplyFlagFrame = 1U << 2;

// True when every |text| fits |cap| (used to reject oversized fields before
// the u32 length cast).
bool Fits(std::string_view text, std::uint32_t cap)
{
  return text.size() <= cap;
}

} // namespace

base::Result<std::string> EncodeSessionRequest(const RendererSessionRequest& request)
{
  if (!Fits(request.url, kMaxUrlBytes) || !Fits(request.content_type, kMaxSessionTextBytes) ||
      !Fits(request.key_type, kMaxSessionTextBytes) || !Fits(request.key, kMaxSessionTextBytes) ||
      !Fits(request.code, kMaxSessionTextBytes) || !Fits(request.document, kMaxDocumentBytes)) {
    return base::Err(base::Error::InvalidArgument("session request field exceeds its size cap"));
  }
  std::string out;
  PutU8(out, kRendererSessionProtocolVersion);
  PutU8(out, static_cast<std::uint8_t>(request.op));
  switch (request.op) {
  case SessionOp::kLoad:
    PutString(out, request.content_type);
    PutString(out, request.url);
    PutString(out, request.document);
    PutU32(out, static_cast<std::uint32_t>(std::max(0, request.viewport_width)));
    PutU32(out, static_cast<std::uint32_t>(std::max(0, request.viewport_height)));
    break;
  case SessionOp::kSnapshot:
    PutU32(out, static_cast<std::uint32_t>(std::max(0, request.viewport_width)));
    PutU32(out, static_cast<std::uint32_t>(std::max(0, request.viewport_height)));
    PutF32(out, request.scroll_y);
    break;
  case SessionOp::kClick:
  case SessionOp::kHover:
    PutF32(out, request.x);
    PutF32(out, request.y);
    break;
  case SessionOp::kScroll:
    PutF32(out, request.scroll_y);
    break;
  case SessionOp::kSetZoom:
    PutF32(out, request.zoom);
    break;
  case SessionOp::kWheel:
    PutF64(out, request.delta_y);
    break;
  case SessionOp::kKey:
    PutString(out, request.key_type);
    PutString(out, request.key);
    PutString(out, request.code);
    break;
  case SessionOp::kHoverClear:
  case SessionOp::kPump:
  case SessionOp::kShutdown:
    break;
  }
  return out;
}

base::Result<RendererSessionRequest> DecodeSessionRequest(std::string_view payload)
{
  Reader reader(payload);
  RendererSessionRequest request;
  std::uint8_t version = 0;
  std::uint8_t op = 0;
  if (auto e = reader.U8(&version); !e.ok()) {
    return base::Err(e);
  }
  if (version != kRendererSessionProtocolVersion) {
    return base::Err(base::Error::InvalidArgument("unsupported session protocol version"));
  }
  if (auto e = reader.U8(&op); !e.ok()) {
    return base::Err(e);
  }
  if (op < static_cast<std::uint8_t>(SessionOp::kLoad) ||
      op > static_cast<std::uint8_t>(SessionOp::kSetZoom)) {
    return base::Err(base::Error::InvalidArgument("unknown session operation"));
  }
  request.op = static_cast<SessionOp>(op);
  switch (request.op) {
  case SessionOp::kLoad: {
    if (auto e = reader.String(kMaxSessionTextBytes, &request.content_type); !e.ok()) {
      return base::Err(e);
    }
    if (auto e = reader.String(kMaxUrlBytes, &request.url); !e.ok()) {
      return base::Err(e);
    }
    if (auto e = reader.String(kMaxDocumentBytes, &request.document); !e.ok()) {
      return base::Err(e);
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (auto e = reader.U32(&width); !e.ok()) {
      return base::Err(e);
    }
    if (auto e = reader.U32(&height); !e.ok()) {
      return base::Err(e);
    }
    request.viewport_width = static_cast<int>(width);
    request.viewport_height = static_cast<int>(height);
    break;
  }
  case SessionOp::kSnapshot: {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (auto e = reader.U32(&width); !e.ok()) {
      return base::Err(e);
    }
    if (auto e = reader.U32(&height); !e.ok()) {
      return base::Err(e);
    }
    if (auto e = reader.F32(&request.scroll_y); !e.ok()) {
      return base::Err(e);
    }
    request.viewport_width = static_cast<int>(width);
    request.viewport_height = static_cast<int>(height);
    break;
  }
  case SessionOp::kClick:
  case SessionOp::kHover:
    if (auto e = reader.F32(&request.x); !e.ok()) {
      return base::Err(e);
    }
    if (auto e = reader.F32(&request.y); !e.ok()) {
      return base::Err(e);
    }
    break;
  case SessionOp::kScroll:
    if (auto e = reader.F32(&request.scroll_y); !e.ok()) {
      return base::Err(e);
    }
    break;
  case SessionOp::kSetZoom:
    if (auto e = reader.F32(&request.zoom); !e.ok()) {
      return base::Err(e);
    }
    break;
  case SessionOp::kWheel:
    if (auto e = reader.F64(&request.delta_y); !e.ok()) {
      return base::Err(e);
    }
    break;
  case SessionOp::kKey:
    if (auto e = reader.String(kMaxSessionTextBytes, &request.key_type); !e.ok()) {
      return base::Err(e);
    }
    if (auto e = reader.String(kMaxSessionTextBytes, &request.key); !e.ok()) {
      return base::Err(e);
    }
    if (auto e = reader.String(kMaxSessionTextBytes, &request.code); !e.ok()) {
      return base::Err(e);
    }
    break;
  case SessionOp::kHoverClear:
  case SessionOp::kPump:
  case SessionOp::kShutdown:
    break;
  }
  if (reader.Remaining() != 0) {
    return base::Err(base::Error::InvalidArgument("trailing bytes in session request"));
  }
  return request;
}

base::Result<std::string> EncodeSessionReply(const RendererSessionReply& reply)
{
  if (!Fits(reply.url, kMaxUrlBytes) || !Fits(reply.redirect_url, kMaxUrlBytes) ||
      !Fits(reply.title, kMaxSessionTextBytes) || !Fits(reply.error, kMaxSessionTextBytes) ||
      !Fits(reply.hover_link, kMaxUrlBytes) || reply.rgba.size() > kMaxRgbaBytes) {
    return base::Err(base::Error::InvalidArgument("session reply field exceeds its size cap"));
  }
  std::uint8_t flags = 0;
  if (reply.handled) {
    flags |= kReplyFlagHandled;
  }
  if (reply.changed) {
    flags |= kReplyFlagChanged;
  }
  if (reply.has_frame) {
    flags |= kReplyFlagFrame;
  }
  std::string out;
  out.reserve(reply.rgba.size() + reply.url.size() + 96);
  PutU8(out, kRendererSessionProtocolVersion);
  PutU8(out, reply.ok ? 0 : 1);
  PutU8(out, flags);
  PutString(out, reply.url);
  PutString(out, reply.title);
  PutString(out, reply.error);
  PutString(out, reply.hover_link);
  PutString(out, reply.redirect_url);
  PutF32(out, reply.content_height);
  PutF32(out, reply.scroll_y);
  PutF32(out, reply.pending_scroll_y);
  PutU64(out, reply.scroll_request_id);
  if (reply.has_frame) {
    PutU32(out, static_cast<std::uint32_t>(reply.width));
    PutU32(out, static_cast<std::uint32_t>(reply.height));
    PutU64(out, static_cast<std::uint64_t>(reply.rgba.size()));
    out.append(reinterpret_cast<const char*>(reply.rgba.data()), reply.rgba.size());
  }
  return out;
}

base::Result<RendererSessionReply> DecodeSessionReply(std::string_view payload)
{
  Reader reader(payload);
  RendererSessionReply reply;
  std::uint8_t version = 0;
  std::uint8_t status = 0;
  std::uint8_t flags = 0;
  if (auto e = reader.U8(&version); !e.ok()) {
    return base::Err(e);
  }
  if (version != kRendererSessionProtocolVersion) {
    return base::Err(base::Error::InvalidArgument("unsupported session protocol version"));
  }
  if (auto e = reader.U8(&status); !e.ok()) {
    return base::Err(e);
  }
  if (auto e = reader.U8(&flags); !e.ok()) {
    return base::Err(e);
  }
  if (auto e = reader.String(kMaxUrlBytes, &reply.url); !e.ok()) {
    return base::Err(e);
  }
  if (auto e = reader.String(kMaxSessionTextBytes, &reply.title); !e.ok()) {
    return base::Err(e);
  }
  if (auto e = reader.String(kMaxSessionTextBytes, &reply.error); !e.ok()) {
    return base::Err(e);
  }
  if (auto e = reader.String(kMaxUrlBytes, &reply.hover_link); !e.ok()) {
    return base::Err(e);
  }
  if (auto e = reader.String(kMaxUrlBytes, &reply.redirect_url); !e.ok()) {
    return base::Err(e);
  }
  if (auto e = reader.F32(&reply.content_height); !e.ok()) {
    return base::Err(e);
  }
  if (auto e = reader.F32(&reply.scroll_y); !e.ok()) {
    return base::Err(e);
  }
  if (auto e = reader.F32(&reply.pending_scroll_y); !e.ok()) {
    return base::Err(e);
  }
  if (auto e = reader.U64(&reply.scroll_request_id); !e.ok()) {
    return base::Err(e);
  }
  reply.ok = status == 0;
  reply.handled = (flags & kReplyFlagHandled) != 0;
  reply.changed = (flags & kReplyFlagChanged) != 0;
  reply.has_frame = (flags & kReplyFlagFrame) != 0;
  if (reply.has_frame) {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t rgba_len = 0;
    if (auto e = reader.U32(&width); !e.ok()) {
      return base::Err(e);
    }
    if (auto e = reader.U32(&height); !e.ok()) {
      return base::Err(e);
    }
    if (auto e = reader.U64(&rgba_len); !e.ok()) {
      return base::Err(e);
    }
    if (rgba_len > kMaxRgbaBytes) {
      return base::Err(base::Error::InvalidArgument("frame exceeds its size cap"));
    }
    const std::uint64_t expected =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4ull;
    if (rgba_len != expected) {
      return base::Err(base::Error::InvalidArgument("frame size does not match its dimensions"));
    }
    const uint8_t* rgba = nullptr;
    if (auto e = reader.Take(static_cast<std::size_t>(rgba_len), &rgba); !e.ok()) {
      return base::Err(e);
    }
    reply.width = static_cast<int>(width);
    reply.height = static_cast<int>(height);
    reply.rgba.assign(rgba, rgba + rgba_len);
  }
  if (reader.Remaining() != 0) {
    return base::Err(base::Error::InvalidArgument("trailing bytes in session reply"));
  }
  return reply;
}

} // namespace neko::browser
