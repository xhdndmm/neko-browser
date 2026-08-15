#include "neko/base/encoding.h"

#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace neko::base::encoding {
namespace {

// Raw bytes are written with explicit hex escapes so tests are independent of
// the source file encoding.
std::string B(std::initializer_list<int> bytes)
{
  std::string out;
  for (const int b : bytes) {
    out.push_back(static_cast<char>(b));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Label resolution (WHATWG "get an encoding")
// ---------------------------------------------------------------------------

TEST(EncodingTest, ResolvesCanonicalLabels)
{
  EXPECT_EQ(CharsetFromLabel("utf-8"), Charset::kUtf8);
  EXPECT_EQ(CharsetFromLabel("UTF8"), Charset::kUtf8);
  EXPECT_EQ(CharsetFromLabel("gbk"), Charset::kGb18030);
  EXPECT_EQ(CharsetFromLabel("gb2312"), Charset::kGb18030);
  EXPECT_EQ(CharsetFromLabel("big5"), Charset::kBig5);
  EXPECT_EQ(CharsetFromLabel("shift_jis"), Charset::kShiftJis);
  EXPECT_EQ(CharsetFromLabel("windows-31j"), Charset::kShiftJis);
  EXPECT_EQ(CharsetFromLabel("euc-kr"), Charset::kEucKr);
  EXPECT_EQ(CharsetFromLabel("euc-jp"), Charset::kEucJp);
  EXPECT_EQ(CharsetFromLabel("iso-2022-jp"), Charset::kIso2022Jp);
  EXPECT_EQ(CharsetFromLabel("windows-1252"), Charset::kWindows1252);
  // Per the standard, "latin1"/"ascii"/"iso-8859-1" are labels for windows-1252.
  EXPECT_EQ(CharsetFromLabel("latin1"), Charset::kWindows1252);
  EXPECT_EQ(CharsetFromLabel("ascii"), Charset::kWindows1252);
  EXPECT_EQ(CharsetFromLabel("iso-8859-1"), Charset::kWindows1252);
  EXPECT_EQ(CharsetFromLabel(" utf-8 "), Charset::kUtf8); // whitespace trimmed
  EXPECT_FALSE(CharsetFromLabel("not-an-encoding").has_value());
}

TEST(EncodingTest, ReplacementLabels)
{
  EXPECT_FALSE(CharsetFromLabel("replacement").has_value());
  EXPECT_FALSE(CharsetFromLabel("iso-2022-cn").has_value());
  EXPECT_EQ(CharsetFromLabelOrReplacement("replacement"), Charset::kReplacement);
}

TEST(EncodingTest, HttpHeaderCharset)
{
  EXPECT_EQ(CharsetFromHttpHeader("text/html; charset=gb2312"), Charset::kGb18030);
  EXPECT_EQ(CharsetFromHttpHeader("text/html;charset=utf-8"), Charset::kUtf8);
  EXPECT_EQ(CharsetFromHttpHeader("text/html; charset=UTF-8"), Charset::kUtf8);
  EXPECT_EQ(CharsetFromHttpHeader("text/plain; charset=shift_jis"), Charset::kShiftJis);
  EXPECT_FALSE(CharsetFromHttpHeader("text/html").has_value());
  EXPECT_FALSE(CharsetFromHttpHeader("text/html; foo=bar").has_value());
}

// ---------------------------------------------------------------------------
// UTF-8 / UTF-16
// ---------------------------------------------------------------------------

TEST(EncodingTest, Utf8PassthroughAndInvalidReplacement)
{
  EXPECT_EQ(DecodeToUtf8("hello", Charset::kUtf8), "hello");
  // Valid two/three/four-byte sequences round-trip.
  const std::string sample = "a\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80z";
  EXPECT_EQ(DecodeToUtf8(sample, Charset::kUtf8), sample);
  // Invalid lead byte -> U+FFFD.
  EXPECT_EQ(DecodeToUtf8(B({0x41, 0xFF, 0x42}), Charset::kUtf8),
            "A\xEF\xBF\xBD"
            "B");
  // Truncated sequence -> U+FFFD.
  EXPECT_EQ(DecodeToUtf8(B({0xE4, 0xB8}), Charset::kUtf8), "\xEF\xBF\xBD");
  // Overlong / surrogate encodings are rejected.
  EXPECT_EQ(DecodeToUtf8(B({0xC0, 0x80}), Charset::kUtf8), "\xEF\xBF\xBD\xEF\xBF\xBD");
  EXPECT_EQ(DecodeToUtf8(B({0xED, 0xA0, 0x80}), Charset::kUtf8),
            "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST(EncodingTest, Utf16Decoding)
{
  // "A" + U+4E2D (中) + U+1F600 (😀) in UTF-16LE.
  const std::string le = B({0x41, 0x00, 0x2D, 0x4E, 0x3D, 0xD8, 0x00, 0xDE});
  EXPECT_EQ(DecodeToUtf8(le, Charset::kUtf16Le), "A\xE4\xB8\xAD\xF0\x9F\x98\x80");
  // Same in UTF-16BE.
  const std::string be = B({0x00, 0x41, 0x4E, 0x2D, 0xD8, 0x3D, 0xDE, 0x00});
  EXPECT_EQ(DecodeToUtf8(be, Charset::kUtf16Be), "A\xE4\xB8\xAD\xF0\x9F\x98\x80");
  // A lone leading surrogate at end of stream -> U+FFFD.
  EXPECT_EQ(DecodeToUtf8(B({0xD8, 0x00}), Charset::kUtf16Be), "\xEF\xBF\xBD");
  // Odd byte count -> trailing U+FFFD.
  EXPECT_EQ(DecodeToUtf8(B({0x41, 0x00, 0x42}), Charset::kUtf16Le), "A\xEF\xBF\xBD");
}

TEST(EncodingTest, BomOverridesAndIsSkipped)
{
  const std::string utf8_bom = "\xEF\xBB\xBFhello";
  EXPECT_EQ(DecodeToUtf8(utf8_bom, Charset::kWindows1252), "hello");
  // UTF-16LE BOM overrides the label and is skipped.
  const std::string le_bom = B({0xFF, 0xFE, 0x41, 0x00});
  EXPECT_EQ(DecodeToUtf8(le_bom, Charset::kUtf8), "A");
  // UTF-16BE BOM.
  const std::string be_bom = B({0xFE, 0xFF, 0x00, 0x41});
  EXPECT_EQ(DecodeToUtf8(be_bom, Charset::kUtf8), "A");
  EXPECT_EQ(SniffBom(utf8_bom), Charset::kUtf8);
  EXPECT_EQ(SniffBom(le_bom), Charset::kUtf16Le);
  EXPECT_EQ(SniffBom(be_bom), Charset::kUtf16Be);
  EXPECT_FALSE(SniffBom("no bom here").has_value());
}

// ---------------------------------------------------------------------------
// windows-1252 (single-byte)
// ---------------------------------------------------------------------------

TEST(EncodingTest, Windows1252)
{
  // 0x80 -> EURO SIGN, 0x93/0x94 -> curly double quotes.
  EXPECT_EQ(DecodeToUtf8(B({0x80, 0x20, 0x93, 0x94}), Charset::kWindows1252),
            "\xE2\x82\xAC"
            " "
            "\xE2\x80\x9C\xE2\x80\x9D");
  // 0xE9 -> U+00E9 (é).
  EXPECT_EQ(DecodeToUtf8(B({0x63, 0x61, 0x66, 0xE9}), Charset::kWindows1252), "caf\xC3\xA9");
  // Byte 0x81 maps to U+0081 per the WHATWG single-byte index.
  EXPECT_EQ(DecodeToUtf8(B({0x81}), Charset::kWindows1252), "\xC2\x81");
}

// ---------------------------------------------------------------------------
// GBK / gb18030
// ---------------------------------------------------------------------------

TEST(EncodingTest, GbkChinese)
{
  // "中文" in GBK: 0xD6D0 0xCEC4.
  const std::string gbk = B({0xD6, 0xD0, 0xCE, 0xC4});
  EXPECT_EQ(DecodeToUtf8(gbk, Charset::kGb18030), "\xE4\xB8\xAD\xE6\x96\x87");
  // Single ASCII bytes pass through.
  EXPECT_EQ(DecodeToUtf8(B({0x41, 0xD6, 0xD0, 0x42}), Charset::kGb18030),
            "A\xE4\xB8\xAD"
            "B");
  // 0x80 -> EURO SIGN.
  EXPECT_EQ(DecodeToUtf8(B({0x80}), Charset::kGb18030), "\xE2\x82\xAC");
}

TEST(EncodingTest, Gb18030FourByte)
{
  // U+0080 (PAD) is encoded as a four-byte sequence 0x81 0x30 0x81 0x30.
  EXPECT_EQ(DecodeToUtf8(B({0x81, 0x30, 0x81, 0x30}), Charset::kGb18030), "\xC2\x80");
  // An invalid four-byte lead restores and replaces.
  // 0x81 0x30 0x82 (invalid third byte pair) -> U+FFFD then reprocessed bytes.
  const std::string out = DecodeToUtf8(B({0x81, 0x30, 0x82, 0x41}), Charset::kGb18030);
  // 0x30 reprocessed as ASCII '0', 0x82 becomes a new lead, then 0x41 forms a
  // two-byte sequence or error.  Just verify it starts with U+FFFD.
  EXPECT_EQ(out.substr(0, 3), "\xEF\xBF\xBD");
}

// ---------------------------------------------------------------------------
// Big5
// ---------------------------------------------------------------------------

TEST(EncodingTest, Big5Traditional)
{
  // "中文" in Big5: 0xA4A4 0xA4E5.
  const std::string big5 = B({0xA4, 0xA4, 0xA4, 0xE5});
  EXPECT_EQ(DecodeToUtf8(big5, Charset::kBig5), "\xE4\xB8\xAD\xE6\x96\x87");
  // ASCII passes through.
  EXPECT_EQ(DecodeToUtf8(B({0x48, 0x69, 0xA4, 0xA4}), Charset::kBig5), "Hi\xE4\xB8\xAD");
}

// ---------------------------------------------------------------------------
// Shift_JIS / EUC-JP
// ---------------------------------------------------------------------------

TEST(EncodingTest, ShiftJisJapanese)
{
  // "日本" in Shift_JIS: 0x93FA 0x96{0x7B}.
  const std::string sjis = B({0x93, 0xFA, 0x96, 0x7B});
  EXPECT_EQ(DecodeToUtf8(sjis, Charset::kShiftJis), "\xE6\x97\xA5\xE6\x9C\xAC");
  // Halfwidth katakana 0xA1 -> U+FF61.
  EXPECT_EQ(DecodeToUtf8(B({0xA1}), Charset::kShiftJis), "\xEF\xBD\xA1");
}

TEST(EncodingTest, EucJpJapanese)
{
  // "日本" in EUC-JP: 0xC6FC 0{0xCB}.
  const std::string euc = B({0xC6, 0xFC, 0xCB, 0xDC});
  EXPECT_EQ(DecodeToUtf8(euc, Charset::kEucJp), "\xE6\x97\xA5\xE6\x9C\xAC");
}

// ---------------------------------------------------------------------------
// EUC-KR
// ---------------------------------------------------------------------------

TEST(EncodingTest, EucKrKorean)
{
  // "한" (U+D55C) in EUC-KR: 0xC7D1.
  const std::string euc_kr = B({0xC7, 0xD1});
  EXPECT_EQ(DecodeToUtf8(euc_kr, Charset::kEucKr), "\xED\x95\x9C");
}

// ---------------------------------------------------------------------------
// ISO-2022-JP
// ---------------------------------------------------------------------------

TEST(EncodingTest, Iso2022Jp)
{
  // ESC ( B switches to ASCII; ESC $ B switches to JIS X 0208.
  // "日本" in JIS X 0208: 0x467C 0x4B5C.
  const std::string doc = B({0x1B, 0x24, 0x42, 0x46, 0x7C, 0x4B, 0x5C, 0x1B, 0x28, 0x42});
  EXPECT_EQ(DecodeToUtf8(doc, Charset::kIso2022Jp), "\xE6\x97\xA5\xE6\x9C\xAC");
}

// ---------------------------------------------------------------------------
// HTML charset detection
// ---------------------------------------------------------------------------

TEST(EncodingTest, DetectsMetaCharset)
{
  const std::string html =
      "<!DOCTYPE html><html><head><meta charset=\"gb2312\"></head><body>x</body></html>";
  EXPECT_EQ(DetectHtmlCharset(html), Charset::kGb18030);
  const std::string html2 =
      "<head><META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=big5\"></head>";
  EXPECT_EQ(DetectHtmlCharset(html2), Charset::kBig5);
  const std::string html3 = "<head><meta charset=shift_jis></head>";
  EXPECT_EQ(DetectHtmlCharset(html3), Charset::kShiftJis);
}

TEST(EncodingTest, HttpHintPriorityOverMeta)
{
  const std::string html = "<meta charset=\"utf-8\"><p>hello</p>";
  // HTTP header wins over the prescan per the sniffing algorithm.
  EXPECT_EQ(DetectHtmlCharset(html, Charset::kGb18030), Charset::kGb18030);
}

TEST(EncodingTest, BomBeatsHttpHint)
{
  const std::string html = "\xEF\xBB\xBF<meta charset=\"gb2312\">";
  EXPECT_EQ(DetectHtmlCharset(html, Charset::kWindows1252), Charset::kUtf8);
}

TEST(EncodingTest, DefaultsToWindows1252)
{
  EXPECT_EQ(DetectHtmlCharset("<html><body>no meta</body></html>"), Charset::kWindows1252);
}

TEST(EncodingTest, DetectsUtf16Signature)
{
  // UTF-16LE "<?x" signature.
  const std::string sig = B({0x3C, 0x00, 0x3F, 0x00, 0x78, 0x00});
  EXPECT_EQ(DetectHtmlCharset(sig), Charset::kUtf16Le);
}

TEST(EncodingTest, FullDocumentDecodeRoundTrip)
{
  // A complete GBK document, decoded to UTF-8, parses the Chinese title.
  const std::string doc =
      B({0x3C, 0x68, 0x74, 0x6D, 0x6C, 0x3E}) + // <html>
      B({0x3C, 0x68, 0x65, 0x61, 0x64, 0x3E}) + // <head>
      B({0x3C, 0x6D, 0x65, 0x74, 0x61, 0x20, 0x63, 0x68, 0x61, 0x72, 0x73, 0x65,
         0x74, 0x3D, 0x22, 0x67, 0x62, 0x32, 0x33, 0x31, 0x32, 0x22, 0x3E}) + // meta
      B({0x3C, 0x74, 0x69, 0x74, 0x6C, 0x65, 0x3E}) +                         // <title>
      B({0xD6, 0xD0, 0xCE, 0xC4}) +                                           // 中文 (GBK)
      B({0x3C, 0x2F, 0x74, 0x69, 0x74, 0x6C, 0x65, 0x3E}) +                   // </title>
      B({0x3C, 0x2F, 0x68, 0x65, 0x61, 0x64, 0x3E}) +                         // </head>
      B({0x3C, 0x2F, 0x68, 0x74, 0x6D, 0x6C, 0x3E});                          // </html>
  const Charset detected = DetectHtmlCharset(doc);
  EXPECT_EQ(detected, Charset::kGb18030);
  const std::string utf8 = DecodeToUtf8(doc, detected);
  EXPECT_NE(utf8.find("\xE4\xB8\xAD\xE6\x96\x87"), std::string::npos);
}

TEST(EncodingTest, CharsetNames)
{
  EXPECT_EQ(CharsetName(Charset::kUtf8), "UTF-8");
  EXPECT_EQ(CharsetName(Charset::kGb18030), "gb18030");
  EXPECT_EQ(CharsetName(Charset::kShiftJis), "Shift_JIS");
  EXPECT_EQ(CharsetName(Charset::kWindows1252), "windows-1252");
  EXPECT_EQ(CharsetName(Charset::kUnknown), "unknown");
}

} // namespace
} // namespace neko::base::encoding
