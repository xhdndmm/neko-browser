// WebSocket protocol implementation (RFC 6455).

#include "neko/network/websocket.h"

#include "neko/base/logging.h"
#include "neko/network/socket.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <random>
#include <sstream>

namespace neko::network {
namespace {

constexpr std::size_t kMaxMessageSize = 16 * 1024 * 1024; // 16 MiB
constexpr char kWebSocketMagicGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

enum Opcode : uint8_t
{
  kContinuation = 0x0,
  kText = 0x1,
  kBinary = 0x2,
  kClose = 0x8,
  kPing = 0x9,
  kPong = 0xA,
};

std::string Base64Encode(std::string_view data)
{
  BIO* b64 = BIO_new(BIO_f_base64());
  BIO* bmem = BIO_new(BIO_s_mem());
  b64 = BIO_push(b64, bmem);
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  BIO_write(b64, data.data(), static_cast<int>(data.size()));
  BIO_flush(b64);
  BUF_MEM* bptr = nullptr;
  BIO_get_mem_ptr(b64, &bptr);
  std::string result(bptr->data, bptr->length);
  BIO_free_all(b64);
  return result;
}

std::string Sha1Hash(std::string_view data)
{
  std::array<unsigned char, SHA_DIGEST_LENGTH> hash = {};
  SHA1(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash.data());
  return std::string(reinterpret_cast<const char*>(hash.data()), hash.size());
}

std::string GenerateWebSocketKey()
{
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint8_t> dist(0, 255);
  std::array<uint8_t, 16> bytes = {};
  for (auto& byte : bytes) {
    byte = dist(gen);
  }
  return Base64Encode(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::string ComputeAcceptKey(std::string_view key)
{
  return Base64Encode(Sha1Hash(std::string(key) + kWebSocketMagicGuid));
}

std::string ToLower(std::string_view s)
{
  std::string r;
  r.reserve(s.size());
  for (char c : s) {
    r.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return r;
}

std::string Trim(std::string_view s)
{
  auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos) {
    return "";
  }
  auto end = s.find_last_not_of(" \t\r\n");
  return std::string(s.substr(start, end - start + 1));
}

} // namespace

WebSocket::WebSocket() = default;

WebSocket::~WebSocket()
{
  if (socket_open_ && !close_sent_) {
    Close(WebSocketCloseCode::GoingAway);
  }
}

WebSocket::WebSocket(WebSocket&& other) noexcept
    : tls_socket_(std::move(other.tls_socket_)), plain_socket_(std::move(other.plain_socket_)),
      use_tls_(other.use_tls_), socket_open_(other.socket_open_), close_sent_(other.close_sent_),
      negotiated_protocol_(std::move(other.negotiated_protocol_)),
      fragment_buffer_(std::move(other.fragment_buffer_)), fragment_type_(other.fragment_type_)
{
  other.socket_open_ = false;
}

WebSocket& WebSocket::operator=(WebSocket&& other) noexcept
{
  if (this != &other) {
    tls_socket_ = std::move(other.tls_socket_);
    plain_socket_ = std::move(other.plain_socket_);
    use_tls_ = other.use_tls_;
    socket_open_ = other.socket_open_;
    close_sent_ = other.close_sent_;
    negotiated_protocol_ = std::move(other.negotiated_protocol_);
    fragment_buffer_ = std::move(other.fragment_buffer_);
    fragment_type_ = other.fragment_type_;
    other.socket_open_ = false;
  }
  return *this;
}

base::Result<std::string> WebSocket::SocketReceive(std::size_t max_bytes, int timeout_ms)
{
  if (use_tls_) {
    return tls_socket_.Receive(max_bytes, timeout_ms);
  }
  return plain_socket_.Receive(max_bytes, timeout_ms);
}

base::Status WebSocket::SocketSend(std::string_view data)
{
  if (use_tls_) {
    auto r = tls_socket_.Send(data);
    if (!r) {
      return base::Err(r.error());
    }
    return base::Ok();
  }
  // Plain socket: Send returns bytes written; loop until all sent.
  std::size_t sent = 0;
  while (sent < data.size()) {
    auto r = plain_socket_.Send(data.substr(sent));
    if (!r) {
      return base::Err(r.error());
    }
    sent += r.value();
  }
  return base::Ok();
}

base::Result<std::string> WebSocket::ReadExactly(std::size_t size, int timeout_ms)
{
  while (read_buffer_.size() < size) {
    // Ask for exactly what the framing still needs: a larger read would block
    // until the deadline waiting for bytes that the next frame has not sent
    // yet, and a polling caller (Receive) would see nothing for a whole tick.
    const std::size_t want = size - read_buffer_.size();
    bool timed_out = false;
    std::string received;
    if (use_tls_) {
      auto outcome = tls_socket_.ReceiveWithOutcome(want, timeout_ms);
      if (!outcome) {
        socket_open_ = false;
        return base::Err(outcome.error());
      }
      timed_out = outcome.value().timed_out;
      received = std::move(outcome.value().data);
    } else {
      auto outcome = plain_socket_.ReceiveWithOutcome(want, timeout_ms);
      if (!outcome) {
        socket_open_ = false;
        return base::Err(outcome.error());
      }
      timed_out = outcome.value().timed_out;
      received = std::move(outcome.value().data);
    }

    // Partial bytes belong to the current frame: keep them even when the
    // deadline interrupted the rest of it, so framing survives the retry.
    if (!received.empty()) {
      read_buffer_ += received;
    }
    if (timed_out) {
      // kIo marks the timeout; callers that poll (WebSocket::Receive) treat it
      // as "nothing yet" instead of a protocol failure.
      return base::Err(base::Error::Io("WebSocket: receive timeout"));
    }
    if (received.empty()) {
      // The peer closed (EOF) without completing the expected frame.
      socket_open_ = false;
      return base::Err(base::Error::Network("WebSocket: connection closed prematurely"));
    }
  }

  std::string out = read_buffer_.substr(0, size);
  read_buffer_.erase(0, size);
  return base::Ok(std::move(out));
}

base::Result<WebSocket> WebSocket::Connect(const url::Url& url,
                                           std::string_view origin,
                                           const std::vector<std::string>& protocols,
                                           const TlsOptions& tls_options,
                                           int timeout_ms)
{
  if (url.scheme() != "ws" && url.scheme() != "wss") {
    return base::Err(base::Error::InvalidArgument("WebSocket URL must use ws:// or wss://"));
  }

  bool use_tls = (url.scheme() == "wss");
  uint16_t port = url.port().value_or(use_tls ? 443u : 80u);

  WebSocket ws;
  ws.use_tls_ = use_tls;

  if (use_tls) {
    TlsOptions opts = tls_options;
    opts.timeout_ms = timeout_ms;
    auto r = TlsSocket::Connect(url.host(), port, opts);
    if (!r) {
      return base::Err(r.error());
    }
    ws.tls_socket_ = std::move(r.value());
  } else {
    auto r = Socket::Connect(url.host(), port, timeout_ms);
    if (!r) {
      return base::Err(r.error());
    }
    ws.plain_socket_ = std::move(r.value());
  }

  std::string key = GenerateWebSocketKey();
  std::string expected_accept = ComputeAcceptKey(key);

  // Build request path: path + query + fragment (though fragment shouldn't be sent).
  std::string request_path = url.path();
  if (url.has_query()) {
    request_path += "?";
    request_path += url.query();
  }
  if (request_path.empty()) {
    request_path = "/";
  }

  std::ostringstream req;
  req << "GET " << request_path << " HTTP/1.1\r\n";
  req << "Host: " << url.host();
  if ((use_tls && port != 443) || (!use_tls && port != 80)) {
    req << ":" << port;
  }
  req << "\r\n";
  req << "Upgrade: websocket\r\n";
  req << "Connection: Upgrade\r\n";
  req << "Sec-WebSocket-Key: " << key << "\r\n";
  req << "Sec-WebSocket-Version: 13\r\n";
  if (!origin.empty()) {
    req << "Origin: " << origin << "\r\n";
  }
  if (!protocols.empty()) {
    req << "Sec-WebSocket-Protocol: ";
    for (std::size_t i = 0; i < protocols.size(); ++i) {
      if (i > 0) {
        req << ", ";
      }
      req << protocols[i];
    }
    req << "\r\n";
  }
  req << "\r\n";

  auto send_status = ws.SocketSend(req.str());
  if (!send_status) {
    return base::Err(send_status.error());
  }

  // Read upgrade response (up to 8 KiB of headers).
  std::string response;
  while (response.size() < 8192) {
    auto chunk = ws.SocketReceive(1, timeout_ms);
    if (!chunk || chunk.value().empty()) {
      return base::Err(base::Error::Network("WebSocket: connection closed during handshake"));
    }
    response.append(chunk.value());
    if (response.size() >= 4 && response.substr(response.size() - 4) == "\r\n\r\n") {
      break;
    }
  }

  auto first_crlf = response.find("\r\n");
  if (first_crlf == std::string::npos ||
      response.substr(0, first_crlf).find("101") == std::string::npos) {
    return base::Err(
        base::Error::Network("WebSocket handshake failed: " + response.substr(0, first_crlf)));
  }

  bool upgrade_ok = false;
  bool connection_ok = false;
  bool accept_ok = false;

  std::istringstream iss(response);
  std::string line;
  std::getline(iss, line); // skip status line
  while (std::getline(iss, line)) {
    if (line == "\r" || line.empty()) {
      break;
    }
    auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string name = ToLower(Trim(line.substr(0, colon)));
    std::string value = Trim(line.substr(colon + 1));

    if (name == "upgrade" && ToLower(value) == "websocket") {
      upgrade_ok = true;
    } else if (name == "connection" && ToLower(value).find("upgrade") != std::string::npos) {
      connection_ok = true;
    } else if (name == "sec-websocket-accept" && value == expected_accept) {
      accept_ok = true;
    } else if (name == "sec-websocket-protocol" && !protocols.empty()) {
      ws.negotiated_protocol_ = value;
    }
  }

  if (!upgrade_ok || !connection_ok || !accept_ok) {
    return base::Err(
        base::Error::Network("WebSocket: handshake validation failed (missing required headers)"));
  }

  ws.socket_open_ = true;
  return base::Ok(std::move(ws));
}

base::Status WebSocket::SendText(std::string_view data)
{
  if (data.size() > kMaxMessageSize) {
    return base::Err(base::Error::InvalidArgument("WebSocket message too large"));
  }
  return SendFrame(kText, data, /*mask=*/true);
}

base::Status WebSocket::SendBinary(std::string_view data)
{
  if (data.size() > kMaxMessageSize) {
    return base::Err(base::Error::InvalidArgument("WebSocket message too large"));
  }
  return SendFrame(kBinary, data, /*mask=*/true);
}

base::Result<WebSocket::Message> WebSocket::Receive(int timeout_ms)
{
  return ReceiveFrame(timeout_ms);
}

base::Status WebSocket::Close(WebSocketCloseCode code, std::string_view reason)
{
  if (close_sent_) {
    return base::Ok();
  }
  std::string payload;
  auto code_int = static_cast<uint16_t>(code);
  payload.push_back(static_cast<char>((code_int >> 8) & 0xFF));
  payload.push_back(static_cast<char>(code_int & 0xFF));
  payload.append(reason);
  auto status = SendFrame(kClose, payload, /*mask=*/true);
  close_sent_ = true;
  socket_open_ = false;
  return status;
}

base::Status WebSocket::SendFrame(uint8_t opcode, std::string_view payload, bool mask)
{
  if (!socket_open_) {
    return base::Err(base::Error::Network("WebSocket: connection is closed"));
  }

  std::string frame;
  frame.push_back(static_cast<char>(0x80u | opcode)); // FIN=1

  std::size_t len = payload.size();
  uint8_t mask_bit = mask ? 0x80u : 0x00u;
  if (len <= 125) {
    frame.push_back(static_cast<char>(mask_bit | len));
  } else if (len <= 65535) {
    frame.push_back(static_cast<char>(mask_bit | 126u));
    frame.push_back(static_cast<char>((len >> 8) & 0xFF));
    frame.push_back(static_cast<char>(len & 0xFF));
  } else {
    frame.push_back(static_cast<char>(mask_bit | 127u));
    for (int i = 7; i >= 0; --i) {
      frame.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
    }
  }

  std::array<uint8_t, 4> masking_key = {};
  if (mask) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    for (auto& b : masking_key) {
      b = dist(gen);
      frame.push_back(static_cast<char>(b));
    }
  }

  for (std::size_t i = 0; i < payload.size(); ++i) {
    uint8_t b = static_cast<uint8_t>(payload[i]);
    if (mask) {
      b ^= masking_key[i % 4];
    }
    frame.push_back(static_cast<char>(b));
  }

  return SocketSend(frame);
}

base::Result<WebSocket::Message> WebSocket::ReceiveFrame(int timeout_ms)
{
  if (!socket_open_) {
    return base::Err(base::Error::Network("WebSocket: connection is closed"));
  }

  auto hdr = ReadExactly(2, timeout_ms);
  if (!hdr) {
    return base::Err(hdr.error());
  }

  uint8_t byte0 = static_cast<uint8_t>(hdr.value()[0]);
  uint8_t byte1 = static_cast<uint8_t>(hdr.value()[1]);
  bool fin = (byte0 & 0x80) != 0;
  uint8_t opcode = byte0 & 0x0F;
  bool masked = (byte1 & 0x80) != 0;
  uint64_t payload_len = byte1 & 0x7F;

  if (payload_len == 126) {
    auto ext = ReadExactly(2, timeout_ms);
    if (!ext) {
      return base::Err(ext.error());
    }
    payload_len = (static_cast<uint64_t>(static_cast<uint8_t>(ext.value()[0])) << 8) |
                  static_cast<uint64_t>(static_cast<uint8_t>(ext.value()[1]));
  } else if (payload_len == 127) {
    auto ext = ReadExactly(8, timeout_ms);
    if (!ext) {
      return base::Err(ext.error());
    }
    payload_len = 0;
    for (int i = 0; i < 8; ++i) {
      payload_len =
          (payload_len << 8) | static_cast<uint64_t>(static_cast<uint8_t>(ext.value()[i]));
    }
  }

  if (payload_len > kMaxMessageSize) {
    socket_open_ = false;
    return base::Err(base::Error::Network("WebSocket: message exceeds 16 MiB limit"));
  }

  std::array<uint8_t, 4> masking_key = {};
  if (masked) {
    auto mk = ReadExactly(4, timeout_ms);
    if (!mk) {
      return base::Err(mk.error());
    }
    for (int i = 0; i < 4; ++i) {
      masking_key[i] = static_cast<uint8_t>(mk.value()[i]);
    }
  }

  std::string payload;
  if (payload_len > 0) {
    auto pd = ReadExactly(static_cast<std::size_t>(payload_len), timeout_ms);
    if (!pd) {
      return base::Err(pd.error());
    }
    payload = pd.value();
    if (masked) {
      for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ masking_key[i % 4]);
      }
    }
  }

  if (opcode == kClose) {
    if (!close_sent_) {
      SendFrame(kClose,
                payload.size() >= 2 ? payload.substr(0, 2) : "",
                /*mask=*/true);
      close_sent_ = true;
    }
    socket_open_ = false;
    return base::Ok(Message{WebSocketMessageType::Text, ""});
  }
  if (opcode == kPing) {
    SendFrame(kPong, payload, /*mask=*/true);
    return ReceiveFrame(timeout_ms);
  }
  if (opcode == kPong) {
    return ReceiveFrame(timeout_ms);
  }

  if (opcode == kText || opcode == kBinary) {
    WebSocketMessageType type =
        (opcode == kText) ? WebSocketMessageType::Text : WebSocketMessageType::Binary;
    if (fin) {
      return base::Ok(Message{type, std::move(payload)});
    }
    fragment_type_ = type;
    fragment_buffer_ = std::move(payload);
    return ReceiveFrame(timeout_ms);
  }
  if (opcode == kContinuation) {
    fragment_buffer_.append(payload);
    if (fin) {
      Message msg{fragment_type_, std::move(fragment_buffer_)};
      fragment_buffer_.clear();
      return base::Ok(std::move(msg));
    }
    return ReceiveFrame(timeout_ms);
  }

  return base::Err(base::Error::Network("WebSocket: unknown opcode"));
}

} // namespace neko::network
