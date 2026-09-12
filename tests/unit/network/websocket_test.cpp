// WebSocket (RFC 6455) unit tests against a local in-process test server.

#include "neko/network/websocket.h"
#include "neko/url/url.h"

#include <atomic>
#include <cstring>
#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace neko::network {
namespace {

#ifndef _WIN32

// Computes the RFC 6455 handshake response for a client key.
std::string AcceptKeyFor(std::string_view key)
{
  const std::string concatenated = std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  unsigned char hash[SHA_DIGEST_LENGTH] = {};
  SHA1(reinterpret_cast<const unsigned char*>(concatenated.data()), concatenated.size(), hash);

  // Base64-encode the digest.
  std::string out;
  out.resize(4 * ((SHA_DIGEST_LENGTH + 2) / 3));
  const int len =
      EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), hash, SHA_DIGEST_LENGTH);
  out.resize(static_cast<std::size_t>(len));
  return out;
}

// Reads from |fd| until |predicate| is satisfied or the deadline elapses.
bool ReadUntil(int fd, std::string* buffer, int timeout_ms)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    char chunk[4096];
    struct pollfd pfd
    {
    };
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int ready = ::poll(&pfd, 1, 50);
    if (ready <= 0) {
      continue;
    }
    const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      return false;
    }
    buffer->append(chunk, static_cast<std::size_t>(n));
    return true;
  }
  return false;
}

// Sends an unmasked server frame (servers must not mask, RFC 6455 §5.1).
void SendServerFrame(int fd, uint8_t opcode, std::string_view payload)
{
  std::string frame;
  frame.push_back(static_cast<char>(0x80 | opcode));
  const std::size_t len = payload.size();
  if (len <= 125) {
    frame.push_back(static_cast<char>(len));
  } else if (len <= 65535) {
    frame.push_back(static_cast<char>(126));
    frame.push_back(static_cast<char>((len >> 8) & 0xFF));
    frame.push_back(static_cast<char>(len & 0xFF));
  } else {
    frame.push_back(static_cast<char>(127));
    for (int i = 7; i >= 0; --i) {
      frame.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
    }
  }
  frame.append(payload);
  ::send(fd, frame.data(), frame.size(), 0);
}

// Decodes one client frame (always masked) into |opcode|/|payload|.
bool ReadClientFrame(int fd, std::string* buffer, uint8_t* opcode, std::string* payload)
{
  // Header.
  while (buffer->size() < 2) {
    if (!ReadUntil(fd, buffer, 2000)) {
      return false;
    }
  }
  const auto byte0 = static_cast<uint8_t>((*buffer)[0]);
  const auto byte1 = static_cast<uint8_t>((*buffer)[1]);
  *opcode = byte0 & 0x0F;
  const bool masked = (byte1 & 0x80) != 0;
  std::size_t payload_len = byte1 & 0x7F;
  std::size_t header_len = 2;

  if (payload_len == 126) {
    while (buffer->size() < 4) {
      if (!ReadUntil(fd, buffer, 2000)) {
        return false;
      }
    }
    payload_len = (static_cast<std::size_t>(static_cast<uint8_t>((*buffer)[2])) << 8) |
                  static_cast<std::size_t>(static_cast<uint8_t>((*buffer)[3]));
    header_len = 4;
  } else if (payload_len == 127) {
    while (buffer->size() < 10) {
      if (!ReadUntil(fd, buffer, 2000)) {
        return false;
      }
    }
    payload_len = 0;
    for (int i = 2; i < 10; ++i) {
      payload_len =
          (payload_len << 8) |
          static_cast<std::size_t>(static_cast<uint8_t>((*buffer)[static_cast<std::size_t>(i)]));
    }
    header_len = 10;
  }

  const std::size_t mask_len = masked ? 4 : 0;
  while (buffer->size() < header_len + mask_len + payload_len) {
    if (!ReadUntil(fd, buffer, 2000)) {
      return false;
    }
  }

  uint8_t mask[4] = {0, 0, 0, 0};
  if (masked) {
    for (std::size_t i = 0; i < 4; ++i) {
      mask[i] = static_cast<uint8_t>((*buffer)[header_len + i]);
    }
  }
  payload->assign(buffer->data() + header_len + mask_len, payload_len);
  if (masked) {
    for (std::size_t i = 0; i < payload->size(); ++i) {
      (*payload)[i] = static_cast<char>(static_cast<uint8_t>((*payload)[i]) ^ mask[i % 4]);
    }
  }
  buffer->erase(0, header_len + mask_len + payload_len);
  return true;
}

// A minimal WebSocket test server: completes the upgrade handshake and then
// runs a scenario-specific interaction.
class TestWebSocketServer
{
public:
  enum class Scenario
  {
    Echo,         // echo every received text message back
    SendGreeting, // send "hello" immediately after the handshake
    SendPing,     // send a ping, expect a pong, then send "after-pong"
    Reject,       // reply 403 instead of upgrading
    BadAccept,    // reply 101 with a wrong Sec-WebSocket-Accept
    Fragmented,   // send "frag" + "mented" as two frames
  };

  explicit TestWebSocketServer(Scenario scenario) : scenario_(scenario)
  {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return;
    }
    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listen_fd_, 4) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { Run(); });
  }

  ~TestWebSocketServer()
  {
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  bool IsValid() const
  {
    return listen_fd_ >= 0;
  }
  uint16_t port() const
  {
    return port_;
  }

private:
  void Run()
  {
    struct pollfd pfd
    {
    };
    pfd.fd = listen_fd_;
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, 5000) <= 0) {
      return;
    }
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      return;
    }
    Handle(fd);
    ::close(fd);
  }

  void Handle(int fd)
  {
    // Read the HTTP upgrade request headers.
    std::string request;
    while (request.find("\r\n\r\n") == std::string::npos) {
      std::string chunk;
      if (!ReadUntil(fd, &chunk, 5000)) {
        return;
      }
      request += chunk;
      if (request.size() > 16384) {
        return;
      }
    }

    if (scenario_ == Scenario::Reject) {
      const std::string body = "rejected";
      const std::string response =
          "HTTP/1.1 403 Forbidden\r\nContent-Length: " + std::to_string(body.size()) +
          "\r\nConnection: close\r\n\r\n" + body;
      ::send(fd, response.data(), response.size(), 0);
      return;
    }

    // Extract Sec-WebSocket-Key.
    std::string key;
    {
      const std::string needle = "sec-websocket-key:";
      std::string lower = request;
      for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      const auto pos = lower.find(needle);
      if (pos == std::string::npos) {
        return;
      }
      auto start = pos + needle.size();
      while (start < request.size() && (request[start] == ' ' || request[start] == '\t')) {
        ++start;
      }
      const auto end = request.find("\r\n", start);
      key = request.substr(start, end - start);
    }

    std::string accept = AcceptKeyFor(key);
    if (scenario_ == Scenario::BadAccept) {
      accept = "AAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    }

    const std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                 "Upgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: " +
                                 accept + "\r\n\r\n";
    ::send(fd, response.data(), response.size(), 0);

    // Run the scenario.
    std::string buffer;
    switch (scenario_) {
    case Scenario::SendGreeting:
      SendServerFrame(fd, 0x1, "hello");
      break;
    case Scenario::Echo: {
      SetupEchoTimeout();
      uint8_t opcode = 0;
      std::string payload;
      while (ReadClientFrame(fd, &buffer, &opcode, &payload)) {
        if (opcode == 0x8) { // close
          SendServerFrame(fd, 0x8, payload.substr(0, 2));
          return;
        }
        if (opcode == 0x1) {
          SendServerFrame(fd, 0x1, payload);
        }
      }
      break;
    }
    case Scenario::SendPing: {
      SendServerFrame(fd, 0x9, "pingdata");
      // Expect a pong.
      uint8_t opcode = 0;
      std::string payload;
      if (ReadClientFrame(fd, &buffer, &opcode, &payload) && opcode == 0xA) {
        SendServerFrame(fd, 0x1, "after-pong");
      }
      break;
    }
    case Scenario::Fragmented:
      // "frag" as a non-final text frame, then "mented" as the final
      // continuation.
      ::send(fd,
             "\x01\x04"
             "frag",
             6,
             0);
      ::send(fd,
             "\x80\x06"
             "mented",
             8,
             0);
      break;
    case Scenario::Reject:
    case Scenario::BadAccept:
      break;
    }
  }

  void SetupEchoTimeout() {}

  Scenario scenario_;
  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::thread thread_;
};

url::Url MakeUrl(uint16_t port, std::string_view path)
{
  auto parsed = url::Url::Parse("ws://127.0.0.1:" + std::to_string(port) + std::string(path));
  EXPECT_TRUE(parsed.has_value());
  return parsed.value();
}

#endif // !_WIN32

TEST(WebSocketTest, RejectsNonWebSocketScheme)
{
  const auto parsed = url::Url::Parse("http://example.com/socket");
  ASSERT_TRUE(parsed.has_value());
  const auto result = WebSocket::Connect(parsed.value(), "");
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kInvalidArgument);
}

#ifndef _WIN32

TEST(WebSocketTest, HandshakeAndSendText)
{
  TestWebSocketServer server(TestWebSocketServer::Scenario::Echo);
  ASSERT_TRUE(server.IsValid());

  auto connected = WebSocket::Connect(MakeUrl(server.port(), "/echo"), "https://example.com");
  ASSERT_TRUE(connected.has_value()) << connected.error().message();
  auto ws = std::move(connected.value());
  EXPECT_TRUE(ws.IsOpen());

  ASSERT_TRUE(ws.SendText("hello server").has_value());

  // Receive the echo.  The server closes the connection afterwards; a close
  // frame surfaces as an empty message.
  auto echoed = ws.Receive(5000);
  ASSERT_TRUE(echoed.has_value()) << echoed.error().message();
  EXPECT_EQ(echoed.value().type, WebSocketMessageType::Text);
  EXPECT_EQ(echoed.value().data, "hello server");

  EXPECT_TRUE(ws.Close().has_value());
}

TEST(WebSocketTest, ReceivesServerInitiatedText)
{
  TestWebSocketServer server(TestWebSocketServer::Scenario::SendGreeting);
  ASSERT_TRUE(server.IsValid());

  auto connected = WebSocket::Connect(MakeUrl(server.port(), "/greet"), "");
  ASSERT_TRUE(connected.has_value()) << connected.error().message();
  auto ws = std::move(connected.value());

  auto message = ws.Receive(5000);
  ASSERT_TRUE(message.has_value()) << message.error().message();
  EXPECT_EQ(message.value().type, WebSocketMessageType::Text);
  EXPECT_EQ(message.value().data, "hello");
}

TEST(WebSocketTest, RepliesToPingWithPong)
{
  TestWebSocketServer server(TestWebSocketServer::Scenario::SendPing);
  ASSERT_TRUE(server.IsValid());

  auto connected = WebSocket::Connect(MakeUrl(server.port(), "/ping"), "");
  ASSERT_TRUE(connected.has_value()) << connected.error().message();
  auto ws = std::move(connected.value());

  // The engine responds to the ping transparently and surfaces the data frame
  // the server sends after the pong.
  auto message = ws.Receive(5000);
  ASSERT_TRUE(message.has_value()) << message.error().message();
  EXPECT_EQ(message.value().data, "after-pong");
}

TEST(WebSocketTest, AssemblesFragmentedMessage)
{
  TestWebSocketServer server(TestWebSocketServer::Scenario::Fragmented);
  ASSERT_TRUE(server.IsValid());

  auto connected = WebSocket::Connect(MakeUrl(server.port(), "/frag"), "");
  ASSERT_TRUE(connected.has_value()) << connected.error().message();
  auto ws = std::move(connected.value());

  auto message = ws.Receive(5000);
  ASSERT_TRUE(message.has_value()) << message.error().message();
  EXPECT_EQ(message.value().type, WebSocketMessageType::Text);
  EXPECT_EQ(message.value().data, "fragmented");
}

TEST(WebSocketTest, RejectedUpgradeFails)
{
  TestWebSocketServer server(TestWebSocketServer::Scenario::Reject);
  ASSERT_TRUE(server.IsValid());

  const auto result = WebSocket::Connect(MakeUrl(server.port(), "/nope"), "");
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kNetwork);
  EXPECT_NE(result.error().message().find("403"), std::string::npos);
}

TEST(WebSocketTest, WrongAcceptKeyIsRejected)
{
  // A server that does not prove knowledge of the client key must be rejected
  // (RFC 6455 §4.1 step 5); otherwise the handshake is trivially forgeable.
  TestWebSocketServer server(TestWebSocketServer::Scenario::BadAccept);
  ASSERT_TRUE(server.IsValid());

  const auto result = WebSocket::Connect(MakeUrl(server.port(), "/bad"), "");
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kNetwork);
}

TEST(WebSocketTest, SendAfterCloseIsRejected)
{
  TestWebSocketServer server(TestWebSocketServer::Scenario::Echo);
  ASSERT_TRUE(server.IsValid());

  auto connected = WebSocket::Connect(MakeUrl(server.port(), "/echo"), "");
  ASSERT_TRUE(connected.has_value()) << connected.error().message();
  auto ws = std::move(connected.value());

  EXPECT_TRUE(ws.Close().has_value());
  const auto send = ws.SendText("too late");
  EXPECT_FALSE(send.has_value());
  EXPECT_EQ(send.error().category(), base::ErrorCategory::kNetwork);
}

TEST(WebSocketTest, OversizedSendIsRejected)
{
  TestWebSocketServer server(TestWebSocketServer::Scenario::SendGreeting);
  ASSERT_TRUE(server.IsValid());

  auto connected = WebSocket::Connect(MakeUrl(server.port(), "/big"), "");
  ASSERT_TRUE(connected.has_value()) << connected.error().message();
  auto ws = std::move(connected.value());

  const std::string too_big(16 * 1024 * 1024 + 1, 'x');
  const auto send = ws.SendText(too_big);
  EXPECT_FALSE(send.has_value());
  EXPECT_EQ(send.error().category(), base::ErrorCategory::kInvalidArgument);
}

#endif // !_WIN32

} // namespace
} // namespace neko::network
