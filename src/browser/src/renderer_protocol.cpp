#include "neko/browser/renderer_protocol.h"

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

} // namespace neko::browser
