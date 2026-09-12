// Tests for the renderer wire protocol (ADR 0016 M1): payload encode/decode
// round trips, malformed-input rejection, and an end-to-end renderer child
// load of a local fixture page.

#include "neko/browser/renderer_host.h"
#include "neko/browser/renderer_protocol.h"

#include <cstdint>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <string>

namespace neko::browser {
namespace {

TEST(RendererProtocolTest, RequestRoundTrip)
{
  RendererLoadRequest request{"https://example.com/page?q=1", 800, 600};
  const auto encoded = EncodeLoadRequest(request);
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeLoadRequest(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value().url, request.url);
  EXPECT_EQ(decoded.value().viewport_width, 800);
  EXPECT_EQ(decoded.value().viewport_height, 600);
}

TEST(RendererProtocolTest, ResultRoundTrip)
{
  RendererLoadResult result;
  result.ok = true;
  result.width = 8;
  result.height = 6;
  result.rgba.assign(8 * 6 * 4, 0x7f);
  result.rgba[0] = 255;
  result.dom = "<html><body>hi</body></html>";
  result.title = "hello";
  const auto encoded = EncodeLoadResult(result);
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeLoadResult(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded.value().ok);
  EXPECT_EQ(decoded.value().width, 8);
  EXPECT_EQ(decoded.value().height, 6);
  ASSERT_EQ(decoded.value().rgba.size(), 8u * 6u * 4u);
  EXPECT_EQ(decoded.value().rgba[0], 255);
  EXPECT_EQ(decoded.value().rgba[1], 0x7f);
  EXPECT_EQ(decoded.value().dom, result.dom);
  EXPECT_EQ(decoded.value().title, "hello");
}

TEST(RendererProtocolTest, ErrorResultRoundTrip)
{
  RendererLoadResult result;
  result.ok = false;
  result.error = "network error: connection refused";
  const auto encoded = EncodeLoadResult(result);
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeLoadResult(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_FALSE(decoded.value().ok);
  EXPECT_EQ(decoded.value().error, result.error);
}

TEST(RendererProtocolTest, RejectsGarbageRequest)
{
  const auto decoded = DecodeLoadRequest("not a real request");
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST(RendererProtocolTest, RejectsWrongVersion)
{
  std::string payload;
  payload.push_back(static_cast<char>(kRendererProtocolVersion + 1));
  payload.push_back(static_cast<char>(kRendererOpLoad));
  const auto decoded = DecodeLoadRequest(payload);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST(RendererProtocolTest, RejectsTrailingBytes)
{
  const auto encoded = EncodeLoadRequest(RendererLoadRequest{"https://a.example/", 1, 1});
  ASSERT_TRUE(encoded.has_value());
  std::string padded = encoded.value();
  padded.push_back('x');
  const auto decoded = DecodeLoadRequest(padded);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST(RendererProtocolTest, RejectsOversizedBitmapClaim)
{
  // A valid prefix (version + ok status + dimensions) followed by a bitmap
  // length beyond the cap must be rejected, not allocated.
  std::string payload;
  payload.push_back(static_cast<char>(kRendererProtocolVersion));
  payload.push_back(0); // ok
  for (int i = 0; i < 4; ++i) {
    payload.push_back(1); // width = 0x01010101
  }
  for (int i = 0; i < 4; ++i) {
    payload.push_back(2); // height = 0x02020202
  }
  // The decoder's bitmap cap is 512 MiB (see renderer_protocol.cpp).
  const std::uint64_t huge = 512ull * 1024ull * 1024ull + 1;
  for (int i = 0; i < 8; ++i) {
    payload.push_back(static_cast<char>((huge >> (i * 8)) & 0xff));
  }
  const auto decoded = DecodeLoadResult(payload);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST(RendererProtocolTest, SelfExecutablePathIsNonEmpty)
{
  EXPECT_FALSE(SelfExecutablePath().empty());
}

// End-to-end: spawn the real browser binary as a renderer child and load a
// local fixture page through the RendererHost.  The child runs the whole
// engine pipeline in its own process and replies with a bitmap + DOM.
TEST(RendererProtocolTest, RendererChildLoadsPageOutOfProcess)
{
  // The renderer child is the browser binary in --renderer-child mode
  // (NEKO_BROWSER_BIN resolves to the built neko_browser executable).
  RendererHost host(NEKO_BROWSER_BIN);
  const std::string target = std::string(NEKO_TEST_PAGES_DIR) + "/button_test.html";
  auto result = host.Load(target, 320, 240);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_TRUE(result.value().ok);
  EXPECT_GT(result.value().width, 0);
  EXPECT_GT(result.value().height, 0);
  ASSERT_EQ(result.value().rgba.size(),
            static_cast<std::size_t>(result.value().width) *
                static_cast<std::size_t>(result.value().height) * 4);
  EXPECT_THAT(result.value().dom, ::testing::HasSubstr("<button"));
  EXPECT_FALSE(result.value().title.empty());
}

// ---------------------------------------------------------------------------
// Session protocol (ADR 0016 M2)
// ---------------------------------------------------------------------------

TEST(RendererSessionProtocolTest, LoadRequestRoundTrip)
{
  RendererSessionRequest request;
  request.op = SessionOp::kLoad;
  request.document = "<html><body>hello</body></html>";
  request.content_type = "text/html; charset=utf-8";
  request.url = "https://example.com/page";
  request.viewport_width = 1024;
  request.viewport_height = 768;
  const auto encoded = EncodeSessionRequest(request);
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeSessionRequest(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value().op, SessionOp::kLoad);
  EXPECT_EQ(decoded.value().document, request.document);
  EXPECT_EQ(decoded.value().content_type, request.content_type);
  EXPECT_EQ(decoded.value().url, request.url);
  EXPECT_EQ(decoded.value().viewport_width, 1024);
  EXPECT_EQ(decoded.value().viewport_height, 768);
}

TEST(RendererSessionProtocolTest, PointerRequestsRoundTrip)
{
  RendererSessionRequest click;
  click.op = SessionOp::kClick;
  click.x = 12.5f;
  click.y = -3.25f;
  auto encoded = EncodeSessionRequest(click);
  ASSERT_TRUE(encoded.has_value());
  auto decoded = DecodeSessionRequest(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value().op, SessionOp::kClick);
  EXPECT_FLOAT_EQ(decoded.value().x, 12.5f);
  EXPECT_FLOAT_EQ(decoded.value().y, -3.25f);

  RendererSessionRequest wheel;
  wheel.op = SessionOp::kWheel;
  wheel.delta_y = 123.75;
  encoded = EncodeSessionRequest(wheel);
  ASSERT_TRUE(encoded.has_value());
  decoded = DecodeSessionRequest(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_DOUBLE_EQ(decoded.value().delta_y, 123.75);

  RendererSessionRequest snapshot;
  snapshot.op = SessionOp::kSnapshot;
  snapshot.viewport_width = 800;
  snapshot.viewport_height = 600;
  snapshot.scroll_y = 42.0f;
  encoded = EncodeSessionRequest(snapshot);
  ASSERT_TRUE(encoded.has_value());
  decoded = DecodeSessionRequest(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value().viewport_width, 800);
  EXPECT_FLOAT_EQ(decoded.value().scroll_y, 42.0f);
}

TEST(RendererSessionProtocolTest, KeyRequestRoundTrip)
{
  RendererSessionRequest request;
  request.op = SessionOp::kKey;
  request.key_type = "keydown";
  request.key = "Enter";
  request.code = "Enter";
  const auto encoded = EncodeSessionRequest(request);
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeSessionRequest(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value().op, SessionOp::kKey);
  EXPECT_EQ(decoded.value().key_type, "keydown");
  EXPECT_EQ(decoded.value().key, "Enter");
  EXPECT_EQ(decoded.value().code, "Enter");
}

TEST(RendererSessionProtocolTest, SimpleOpsRoundTrip)
{
  for (const SessionOp op : {SessionOp::kHoverClear, SessionOp::kPump, SessionOp::kShutdown}) {
    RendererSessionRequest request;
    request.op = op;
    const auto encoded = EncodeSessionRequest(request);
    ASSERT_TRUE(encoded.has_value());
    const auto decoded = DecodeSessionRequest(encoded.value());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value().op, op);
  }
}

TEST(RendererSessionProtocolTest, ReplyWithoutFrameRoundTrip)
{
  RendererSessionReply reply;
  reply.ok = true;
  reply.handled = true;
  reply.changed = false;
  reply.url = "https://example.com/after";
  reply.title = "After";
  reply.content_height = 1234.5f;
  reply.scroll_y = 50.0f;
  reply.pending_scroll_y = 75.0f;
  reply.scroll_request_id = 7;
  reply.hover_link = "https://example.com/link";
  reply.redirect_url = "https://example.com/child-nav";
  const auto encoded = EncodeSessionReply(reply);
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeSessionReply(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded.value().ok);
  EXPECT_TRUE(decoded.value().handled);
  EXPECT_FALSE(decoded.value().changed);
  EXPECT_FALSE(decoded.value().has_frame);
  EXPECT_EQ(decoded.value().url, reply.url);
  EXPECT_EQ(decoded.value().title, reply.title);
  EXPECT_FLOAT_EQ(decoded.value().content_height, 1234.5f);
  EXPECT_FLOAT_EQ(decoded.value().scroll_y, 50.0f);
  EXPECT_FLOAT_EQ(decoded.value().pending_scroll_y, 75.0f);
  EXPECT_EQ(decoded.value().scroll_request_id, 7u);
  EXPECT_EQ(decoded.value().hover_link, reply.hover_link);
  EXPECT_EQ(decoded.value().redirect_url, reply.redirect_url);
}

TEST(RendererSessionProtocolTest, ReplyWithFrameRoundTrip)
{
  RendererSessionReply reply;
  reply.ok = true;
  reply.changed = true;
  reply.has_frame = true;
  reply.width = 4;
  reply.height = 2;
  reply.rgba.assign(4u * 2u * 4u, 0x11);
  reply.rgba[3] = 0xff;
  const auto encoded = EncodeSessionReply(reply);
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeSessionReply(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded.value().has_frame);
  EXPECT_EQ(decoded.value().width, 4);
  EXPECT_EQ(decoded.value().height, 2);
  EXPECT_EQ(decoded.value().rgba, reply.rgba);
}

TEST(RendererSessionProtocolTest, ErrorReplyRoundTrip)
{
  RendererSessionReply reply;
  reply.ok = false;
  reply.error = "no document to rasterize";
  const auto encoded = EncodeSessionReply(reply);
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeSessionReply(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_FALSE(decoded.value().ok);
  EXPECT_EQ(decoded.value().error, reply.error);
}

TEST(RendererSessionProtocolTest, RejectsUnknownOp)
{
  std::string payload;
  payload.push_back(static_cast<char>(kRendererSessionProtocolVersion));
  payload.push_back(static_cast<char>(99));
  const auto decoded = DecodeSessionRequest(payload);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST(RendererSessionProtocolTest, RejectsWrongSessionVersion)
{
  std::string payload;
  payload.push_back(static_cast<char>(kRendererSessionProtocolVersion + 1));
  payload.push_back(static_cast<char>(SessionOp::kPump));
  const auto decoded = DecodeSessionRequest(payload);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST(RendererSessionProtocolTest, RejectsTrailingBytes)
{
  RendererSessionRequest request;
  request.op = SessionOp::kPump;
  const auto encoded = EncodeSessionRequest(request);
  ASSERT_TRUE(encoded.has_value());
  std::string padded = encoded.value();
  padded.push_back('x');
  const auto decoded = DecodeSessionRequest(padded);
  ASSERT_FALSE(decoded.has_value());
}

TEST(RendererSessionProtocolTest, RejectsFrameSizeMismatch)
{
  // A reply claiming a frame whose byte count does not match its dimensions
  // must be rejected, not read past.
  RendererSessionReply reply;
  reply.ok = true;
  reply.has_frame = true;
  reply.width = 4;
  reply.height = 2;
  reply.rgba.assign(4, 0); // 4 bytes instead of 32
  const auto encoded = EncodeSessionReply(reply);
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeSessionReply(encoded.value());
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST(RendererSessionProtocolTest, RejectsOversizedDocument)
{
  RendererSessionRequest request;
  request.op = SessionOp::kLoad;
  request.document.assign(32u * 1024u * 1024u + 1, 'x');
  const auto encoded = EncodeSessionRequest(request);
  ASSERT_FALSE(encoded.has_value());
  EXPECT_EQ(encoded.error().category(), base::ErrorCategory::kInvalidArgument);
}

} // namespace
} // namespace neko::browser
