#pragma once

#include "neko/base/status.h"
#include "neko/network/socket.h"
#include "neko/network/tls_socket.h"
#include "neko/url/url.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace neko::network {

// WebSocket message types (RFC 6455).
enum class WebSocketMessageType
{
  Text,
  Binary,
};

// WebSocket close codes (RFC 6455 §7.4).
enum class WebSocketCloseCode : uint16_t
{
  Normal = 1000,
  GoingAway = 1001,
  ProtocolError = 1002,
  UnsupportedData = 1003,
  NoStatusReceived = 1005,
  AbnormalClosure = 1006,
  InvalidPayload = 1007,
  PolicyViolation = 1008,
  MessageTooBig = 1009,
  InternalError = 1011,
};

// A WebSocket connection (RFC 6455).
//
// Threading: confined to one thread (the JavaScript engine thread in
// practice).  The connection is blocking; send/receive operations block until
// complete or timeout.
//
// Documented approximations:
//   * No automatic ping/pong keep-alive (Phase 8 M4 scope).
//   * Message size limited to 16 MiB (prevents memory exhaustion).
//   * No fragmented message assembly beyond the initial implementation.
//   * Synchronous blocking I/O (matches the page's synchronous execution).
class WebSocket
{
public:
  WebSocket();
  ~WebSocket();

  WebSocket(WebSocket&& other) noexcept;
  WebSocket& operator=(WebSocket&& other) noexcept;
  WebSocket(const WebSocket&) = delete;
  WebSocket& operator=(const WebSocket&) = delete;

  // Establishes a WebSocket connection to |url| (ws:// or wss://).  Performs
  // HTTP upgrade handshake with Sec-WebSocket-Key/Accept validation.
  // |origin| is sent in the handshake Origin header for CORS validation.
  // |protocols| is the list of subprotocols requested (may be empty).
  // Returns the negotiated subprotocol on success (may be empty).
  static base::Result<WebSocket> Connect(const url::Url& url,
                                         std::string_view origin,
                                         const std::vector<std::string>& protocols = {},
                                         const TlsOptions& tls_options = {},
                                         int timeout_ms = 30000);

  // Sends a text or binary message.  |data| must be ≤16 MiB.
  base::Status SendText(std::string_view data);
  base::Status SendBinary(std::string_view data);

  // Receives the next message (blocks until available or timeout).  Returns
  // the message type and data.  An empty result with type Text means the peer
  // closed the connection cleanly.
  struct Message
  {
    WebSocketMessageType type;
    std::string data;
  };
  base::Result<Message> Receive(int timeout_ms = 10000);

  // Sends a close frame with |code| and optional |reason|, then waits for the
  // peer's close frame.  Idempotent (subsequent calls are no-ops).
  base::Status Close(WebSocketCloseCode code = WebSocketCloseCode::Normal,
                     std::string_view reason = "");

  bool IsOpen() const
  {
    return socket_open_;
  }

  const std::string& NegotiatedProtocol() const
  {
    return negotiated_protocol_;
  }

private:
  // Internal frame send/receive.
  base::Status SendFrame(uint8_t opcode, std::string_view payload, bool mask);
  base::Result<Message> ReceiveFrame(int timeout_ms);

  // Helper methods for socket I/O abstraction.
  base::Result<std::string> SocketReceive(std::size_t max_bytes, int timeout_ms);
  base::Status SocketSend(std::string_view data);
  base::Result<std::string> ReadExactly(std::size_t size, int timeout_ms);

  // The underlying socket (either TLS or plain TCP).
  TlsSocket tls_socket_;
  Socket plain_socket_;
  bool use_tls_ = false;
  bool socket_open_ = false;
  bool close_sent_ = false;

  std::string negotiated_protocol_;

  // Bytes received from the socket but not yet consumed by ReadExactly.  A
  // timeout can interrupt a frame mid-read; keeping the partial bytes here
  // preserves framing across Receive() calls that report a timeout.
  std::string read_buffer_;

  // Fragmented message assembly buffer.
  std::string fragment_buffer_;
  WebSocketMessageType fragment_type_ = WebSocketMessageType::Text;
};

} // namespace neko::network
