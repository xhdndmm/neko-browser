// Unit tests for neko::ipc: frame encoding/decoding and Channel round-trips
// over an in-process pipe pair (ADR 0016).

#include "neko/ipc/channel.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>

// Writing to a pipe whose peer is gone must fail with EPIPE, not SIGPIPE.
const int kSigpipeIgnored = [] {
  std::signal(SIGPIPE, SIG_IGN);
  return 0;
}();
#endif

namespace neko::ipc {
namespace {

// Creates a connected pipe pair with one end as the parent channel and the
// other as a second Channel (same process, both ends open).
#ifdef _WIN32
struct PipePair
{
  Channel a;
  Channel b;
};

PipePair MakePipePair()
{
  HANDLE a_read = nullptr;
  HANDLE a_write = nullptr;
  HANDLE b_read = nullptr;
  HANDLE b_write = nullptr;
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  EXPECT_TRUE(CreatePipe(&a_read, &b_write, &sa, 0));
  EXPECT_TRUE(CreatePipe(&b_read, &a_write, &sa, 0));
  return {Channel::FromHandles(a_read, a_write), Channel::FromHandles(b_read, b_write)};
}
#else
struct PipePair
{
  Channel a;
  Channel b;
};

PipePair MakePipePair()
{
  int fds1[2] = {-1, -1};
  int fds2[2] = {-1, -1};
  EXPECT_EQ(::pipe(fds1), 0);
  EXPECT_EQ(::pipe(fds2), 0);
  // a writes into fds1[1], b reads fds1[0]; b writes fds2[1], a reads fds2[0].
  return {Channel::FromHandles(fds2[0], fds1[1]), Channel::FromHandles(fds1[0], fds2[1])};
}
#endif

TEST(FrameTest, RoundTripPreservesPayload)
{
  const std::string payload = "hello renderer \x00\x01 binary";
  const auto encoded = EncodeFrame(payload);
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeFrame(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value(), payload);
}

TEST(FrameTest, EmptyPayloadRoundTrip)
{
  const auto encoded = EncodeFrame("");
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded.value().size(), 4u);
  const auto decoded = DecodeFrame(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded.value().empty());
}

TEST(FrameTest, RejectsTruncatedHeader)
{
  const auto decoded = DecodeFrame("\x01\x02");
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST(FrameTest, RejectsOversizedFrameLength)
{
  // Header claims 100 MiB (> cap) with no body.
  std::string frame(4, '\0');
  const std::uint32_t huge = 100u * 1024u * 1024u;
  frame[0] = static_cast<char>(huge & 0xff);
  frame[1] = static_cast<char>((huge >> 8) & 0xff);
  frame[2] = static_cast<char>((huge >> 16) & 0xff);
  frame[3] = static_cast<char>((huge >> 24) & 0xff);
  const auto decoded = DecodeFrame(frame);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST(FrameTest, RejectsTruncatedBody)
{
  // Header claims 10 bytes but only 3 follow.
  std::string frame = "\x0a\x00\x00\x00"
                      "abc";
  const auto decoded = DecodeFrame(frame);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST(FrameTest, EncodeRejectsPayloadOverCap)
{
  const std::string huge(kMaxFrameBytes + 1, 'x');
  const auto encoded = EncodeFrame(huge);
  ASSERT_FALSE(encoded.has_value());
  EXPECT_EQ(encoded.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST(ChannelTest, SendReceiveRoundTrip)
{
  PipePair pair = MakePipePair();
  const std::string message = "frame one";
  EXPECT_TRUE(pair.a.Send(message));
  const auto received = pair.b.Receive();
  ASSERT_TRUE(received.has_value());
  EXPECT_EQ(received.value(), message);
}

TEST(ChannelTest, BidirectionalExchange)
{
  PipePair pair = MakePipePair();
  EXPECT_TRUE(pair.a.Send("ping"));
  EXPECT_TRUE(pair.b.Send("pong"));
  const auto from_a = pair.b.Receive();
  const auto from_b = pair.a.Receive();
  ASSERT_TRUE(from_a.has_value());
  ASSERT_TRUE(from_b.has_value());
  EXPECT_EQ(from_a.value(), "ping");
  EXPECT_EQ(from_b.value(), "pong");
}

TEST(ChannelTest, MultipleFramesBackToBack)
{
  PipePair pair = MakePipePair();
  EXPECT_TRUE(pair.a.Send("first"));
  EXPECT_TRUE(pair.a.Send("second"));
  const auto first = pair.b.Receive();
  const auto second = pair.b.Receive();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(first.value(), "first");
  EXPECT_EQ(second.value(), "second");
}

TEST(ChannelTest, LargeFrameWithinCap)
{
  PipePair pair = MakePipePair();
  const std::string message(1u << 20, 'z'); // 1 MiB, larger than the pipe buffer
  // The frame exceeds the pipe capacity, so the writer blocks until the
  // reader consumes; exchange on a thread to avoid a self-deadlock.
  std::string received;
  std::thread reader([&] {
    const auto r = pair.b.Receive();
    if (r.has_value()) {
      received = r.value();
    }
  });
  EXPECT_TRUE(pair.a.Send(message));
  reader.join();
  EXPECT_EQ(received, message);
}

TEST(ChannelTest, PeerClosedIsReportedAsIoError)
{
  PipePair pair = MakePipePair();
  pair.b.Close(); // peer closed its end
  const auto received = pair.a.Receive();
  ASSERT_FALSE(received.has_value());
  EXPECT_EQ(received.error().category(), base::ErrorCategory::kIo);
}

TEST(ChannelTest, SendFailsAfterPeerClosed)
{
  PipePair pair = MakePipePair();
  pair.b.Close();
  // The first write may still succeed on some platforms (pipe buffers);
  // consume up to a few frames until the write fails, which it must
  // eventually once the peer is gone.
  bool failed = false;
  for (int i = 0; i < 100 && !failed; ++i) {
    const std::string message(1u << 16, 'x');
    if (!pair.a.Send(message)) {
      failed = true;
    }
  }
  EXPECT_TRUE(failed);
}

} // namespace
} // namespace neko::ipc
