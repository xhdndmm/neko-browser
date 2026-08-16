// Unit tests for neko::pdf text extraction.  Test PDFs are assembled by a
// small in-test builder (real xref table, indirect objects, streams), so the
// extractor is exercised against genuine PDF structure.

#include "neko/base/status.h"
#include "neko/pdf/pdf.h"

#include <cstdint>
#include <cstdio>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>
#include <zlib.h>

namespace neko::pdf {
namespace {

// ---------------------------------------------------------------------------
// Minimal PDF builder (test-only)
// ---------------------------------------------------------------------------

std::string Deflate(std::string_view data)
{
  uLongf bound = compressBound(static_cast<uLong>(data.size()));
  std::vector<Bytef> out(bound);
  uLongf out_size = bound;
  if (compress2(out.data(),
                &out_size,
                reinterpret_cast<const Bytef*>(data.data()),
                static_cast<uLong>(data.size()),
                9) != Z_OK) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(out.data()), out_size);
}

// Applies a PNG "Up"-style predictor row prefix (filter 0) — used to build
// a FlateDecode stream with /Predictor 12.
std::string PngPredictRows(std::string_view data, int columns, int bpp)
{
  std::string out;
  const size_t col_bytes = static_cast<size_t>(columns) * static_cast<size_t>(bpp);
  const int height = static_cast<int>(data.size()) / static_cast<int>(col_bytes);
  for (int y = 0; y < height; ++y) {
    out.push_back(0);
    out.append(data.data() + static_cast<size_t>(y) * col_bytes, col_bytes);
  }
  return out;
}

constexpr const char* kHeader = "%PDF-1.4\n";

class PdfBuilder
{
public:
  // The header must be part of body_ from the start so recorded offsets
  // (which include it) match the absolute file offsets.
  PdfBuilder()
  {
    body_ = kHeader;
  }

  int Add(int num, const std::string& body)
  {
    const size_t unum = static_cast<size_t>(num);
    if (unum >= offsets_.size()) {
      offsets_.resize(unum + 1, ~size_t{0}); // ~0 = unset sentinel
    }
    offsets_[unum] = body_.size();
    body_ += std::to_string(num) + " 0 obj\n" + body + "\nendobj\n";
    return num;
  }

  // Adds a stream object with the given /Filter ("" = none).
  int AddStream(int num,
                std::string_view content,
                std::string_view filter = "FlateDecode",
                const std::string& decode_parms = "")
  {
    std::string data(content);
    std::string dict = "<< /Length ";
    if (filter == "FlateDecode") {
      data = Deflate(content);
      dict += std::to_string(data.size());
      dict += " /Filter /FlateDecode";
      if (!decode_parms.empty())
        dict += " /DecodeParms " + decode_parms;
    } else {
      dict += std::to_string(data.size());
    }
    dict += " >>\nstream\n";
    Add(num, dict + data + "\nendstream");
    return num;
  }

  std::string Finish(const std::string& trailer_extra = "", int trailer_size = -1)
  {
    const int size = trailer_size >= 0 ? trailer_size : static_cast<int>(offsets_.size());
    const size_t xref_offset = body_.size();
    std::string xref = "xref\n0 " + std::to_string(size) + "\n";
    for (int i = 0; i < size; ++i) {
      const size_t ui = static_cast<size_t>(i);
      if (i == 0 || ui >= offsets_.size() || offsets_[ui] == ~size_t{0}) {
        xref += "0000000000 65535 f \n";
      } else {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", offsets_[ui]);
        xref += buf;
      }
    }
    body_ += xref;
    body_ += "trailer\n<< /Size " + std::to_string(size) + " /Root 1 0 R" + trailer_extra +
             " >>\nstartxref\n" + std::to_string(xref_offset) + "\n%%EOF\n";
    return body_;
  }

  std::string body_;
  std::vector<size_t> offsets_;
};
// Builds a simple N-page PDF where each page shows one content stream.
std::string BuildSimplePdf(const std::vector<std::string>& page_contents,
                           const std::string& title = "Test Document",
                           bool flate = true,
                           int predictor = 0)
{
  PdfBuilder b;
  b.Add(1, "<< /Type /Catalog /Pages 2 0 R >>");
  std::string kids;
  const int first_page = 3;
  const int content_start = first_page + static_cast<int>(page_contents.size());
  for (size_t i = 0; i < page_contents.size(); ++i) {
    kids += std::to_string(first_page + static_cast<int>(i)) + " 0 R ";
  }
  b.Add(2,
        "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(page_contents.size()) +
            " >>");
  for (size_t i = 0; i < page_contents.size(); ++i) {
    b.Add(first_page + static_cast<int>(i),
          "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents " +
              std::to_string(content_start + static_cast<int>(i)) + " 0 R >>");
  }
  for (size_t i = 0; i < page_contents.size(); ++i) {
    std::string parms;
    if (predictor > 0) {
      const std::string content = page_contents[i];
      // columns=5 divides the 45-byte test content evenly (no truncation).
      const std::string predicted = PngPredictRows(content, 5, 1); // 5 "columns"
      std::string dict = "<< /Predictor 12 /Columns 5 /Colors 1 /BitsPerComponent 8 >>";
      // Reuse AddStream with a pre-predicted body: encode manually here.
      std::string compressed = Deflate(predicted);
      std::string body = "<< /Length " + std::to_string(compressed.size()) +
                         " /Filter /FlateDecode /DecodeParms " + dict + " >>\nstream\n" +
                         compressed + "\nendstream";
      b.Add(content_start + static_cast<int>(i), body);
      continue;
    }
    b.AddStream(content_start + static_cast<int>(i), page_contents[i], flate ? "FlateDecode" : "");
  }
  std::string info;
  if (!title.empty()) {
    info += " /Info << /Title (" + title + ") >>";
  }
  return b.Finish(info);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(PdfTest, ExtractsSimpleText)
{
  const std::string pdf = BuildSimplePdf({"BT /F1 12 Tf 72 720 Td (Hello World) Tj ET"});
  ASSERT_TRUE(IsPdf(pdf));
  auto r = ExtractText(pdf);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  ASSERT_EQ(r.value().page_count, 1);
  ASSERT_EQ(r.value().pages.size(), 1u);
  EXPECT_THAT(r.value().pages[0].text, testing::HasSubstr("Hello World"));
  EXPECT_EQ(r.value().pages[0].width, 612);
  EXPECT_EQ(r.value().pages[0].height, 792);
  EXPECT_EQ(r.value().title, "Test Document");
}

TEST(PdfTest, ExtractsUncompressedContent)
{
  const std::string pdf =
      BuildSimplePdf({"BT /F1 12 Tf 72 720 Td (Plain Stream) Tj ET"}, "T", false);
  auto r = ExtractText(pdf);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  EXPECT_THAT(r.value().pages[0].text, testing::HasSubstr("Plain Stream"));
}

TEST(PdfTest, MultiPageOrder)
{
  const std::string pdf = BuildSimplePdf(
      {"BT /F1 12 Tf 72 720 Td (Page One) Tj ET", "BT /F1 12 Tf 72 720 Td (Page Two) Tj ET"});
  auto r = ExtractText(pdf);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  ASSERT_EQ(r.value().page_count, 2);
  EXPECT_THAT(r.value().pages[0].text, testing::HasSubstr("Page One"));
  EXPECT_THAT(r.value().pages[1].text, testing::HasSubstr("Page Two"));
}

TEST(PdfTest, LineBreaksOnMove)
{
  const std::string pdf =
      BuildSimplePdf({"BT /F1 12 Tf 72 720 Td (First Line) Tj 0 -14 Td (Second Line) Tj ET"});
  auto r = ExtractText(pdf);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  EXPECT_THAT(r.value().pages[0].text, testing::HasSubstr("First Line"));
  EXPECT_THAT(r.value().pages[0].text, testing::HasSubstr("Second Line"));
  EXPECT_NE(r.value().pages[0].text.find("First Line"),
            r.value().pages[0].text.find("Second Line"));
}

TEST(PdfTest, TjArray)
{
  const std::string pdf = BuildSimplePdf({"BT /F1 12 Tf 72 720 Td [(Hello) 20 (World)] TJ ET"});
  auto r = ExtractText(pdf);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  EXPECT_THAT(r.value().pages[0].text, testing::HasSubstr("Hello World"));
}

TEST(PdfTest, EscapedStrings)
{
  const std::string pdf = BuildSimplePdf({"BT /F1 12 Tf 72 720 Td (A \\(B\\) C) Tj ET"});
  auto r = ExtractText(pdf);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  EXPECT_THAT(r.value().pages[0].text, testing::HasSubstr("A (B) C"));
}

TEST(PdfTest, Utf16BeText)
{
  // Build with explicit length: the literal contains NUL bytes and must not
  // be truncated by the C-string constructor.
  const char bytes[] = "BT /F1 12 Tf 72 720 Td (\xFE\xFF\x00H\x00i\x00!) Tj ET";
  const std::string content(bytes, sizeof(bytes) - 1);
  const std::string pdf = BuildSimplePdf({content}, "T");
  auto r = ExtractText(pdf);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  EXPECT_THAT(r.value().pages[0].text, testing::HasSubstr("Hi!"));
}

TEST(PdfTest, PngPredictorStream)
{
  // Build a content stream with a PNG (type 12) predictor.
  const std::string content = "BT /F1 12 Tf 72 720 Td (Predicted Text) Tj ET";
  const std::string pdf = BuildSimplePdf({content}, "T", true, /*predictor=*/12);
  auto r = ExtractText(pdf);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  EXPECT_THAT(r.value().pages[0].text, testing::HasSubstr("Predicted Text"));
}

TEST(PdfTest, FollowsPrevXrefChain)
{
  // A two-revision (incremental-update) PDF: the content stream object only
  // exists in revision 1, and revision 2's xref points /Prev at revision 1.
  // Offsets are computed manually against the combined file.
  auto pad = [](size_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%010zu", v);
    return std::string(buf);
  };
  std::string file = "%PDF-1.4\n";

  const size_t o1 = file.size();
  file += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
  const size_t o2 = file.size();
  file += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
  const size_t o3 = file.size();
  file +=
      "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 4 0 R >>\nendobj\n";
  const size_t o4 = file.size();
  const std::string c1 = Deflate("BT /F1 12 Tf 10 10 Td (First Revision) Tj ET");
  file += "4 0 obj\n<< /Length " + std::to_string(c1.size()) +
          " /Filter /FlateDecode >>\nstream\n" + c1 + "\nendstream\nendobj\n";
  const size_t x1 = file.size();
  file += "xref\n0 5\n0000000000 65535 f \n";
  file += pad(o1) + " 00000 n \n";
  file += pad(o2) + " 00000 n \n";
  file += pad(o3) + " 00000 n \n";
  file += pad(o4) + " 00000 n \n";
  file += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + std::to_string(x1) + "\n%%EOF\n";

  // Revision 2: updated catalog page tree (objects 2, 6) and a new content
  // stream (object 7).  Object 4 is unchanged and not re-listed.
  const size_t o2b = file.size();
  file += "2 0 obj\n<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >>\nendobj\n";
  const size_t o6 = file.size();
  file +=
      "6 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 7 0 R >>\nendobj\n";
  const size_t o7 = file.size();
  const std::string c2 = Deflate("BT /F1 12 Tf 10 10 Td (Second Revision) Tj ET");
  file += "7 0 obj\n<< /Length " + std::to_string(c2.size()) +
          " /Filter /FlateDecode >>\nstream\n" + c2 + "\nendstream\nendobj\n";
  const size_t x2 = file.size();
  file += "xref\n0 8\n0000000000 65535 f \n";
  file += pad(o1) + " 00000 n \n";
  file += pad(o2b) + " 00000 n \n";
  file += "0000000000 65535 f \n";
  file += "0000000000 65535 f \n";
  file += "0000000000 65535 f \n";
  file += pad(o6) + " 00000 n \n";
  file += pad(o7) + " 00000 n \n";
  file += "trailer\n<< /Size 8 /Root 1 0 R /Prev " + std::to_string(x1) + " >>\nstartxref\n" +
          std::to_string(x2) + "\n%%EOF\n";

  auto r = ExtractText(file);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  ASSERT_EQ(r.value().page_count, 2);
  // Page 1's content stream lives only in revision 1 (via /Prev).
  EXPECT_THAT(r.value().pages[0].text, testing::HasSubstr("First Revision"));
  EXPECT_THAT(r.value().pages[1].text, testing::HasSubstr("Second Revision"));
}

TEST(PdfTest, RejectsNonPdf)
{
  auto r = ExtractText("This is definitely not a PDF file.");
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(r.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST(PdfTest, RejectsMissingXref)
{
  const std::string pdf = std::string(kHeader) + "%PDF without any cross reference table\n";
  auto r = ExtractText(pdf);
  EXPECT_FALSE(r.has_value());
}

TEST(PdfTest, MissingInfoTitleIsEmpty)
{
  const std::string pdf = BuildSimplePdf({"BT /F1 12 Tf 1 1 Td (X) Tj ET"}, "");
  auto r = ExtractText(pdf);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  EXPECT_TRUE(r.value().title.empty());
}

} // namespace
} // namespace neko::pdf
