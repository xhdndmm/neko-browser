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

} // namespace
} // namespace neko::browser
