// Minimal PDF extractor + page renderer.
//
// Implements just enough of the PDF 1.7 spec to extract text from ordinary
// documents and rasterize a page: the classic xref table (with /Prev chains),
// the object model, streams with FlateDecode (PNG/TIFF predictors), the pages
// tree and the core text/graphics operators.  See pdf.h for the explicit
// NOT IMPLEMENTED lists.
//
// Threading: pure functions, no shared state (each RenderPage/ExtractText
// call parses its own document).

#include "neko/base/status.h"
#include "neko/base/utf8.h"
#include "neko/graphics/font_face.h"
#include "neko/graphics/font_registry.h"
#include "neko/graphics/font_selector.h"
#include "neko/pdf/pdf.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <zlib.h>

namespace neko::pdf {
namespace {

// ---------------------------------------------------------------------------
// Object model
//
// Recursive containers (arrays, dicts) hold shared_ptr<PdfObject> so the
// variant can be defined without a complete PdfObject.
// ---------------------------------------------------------------------------

struct PdfName
{
  std::string value;
};
struct PdfString
{
  std::string value;
};
struct PdfRef
{
  int64_t num = 0;
  int64_t gen = 0;
};
struct PdfObject;
using PdfObjectPtr = std::shared_ptr<PdfObject>;
struct PdfArray
{
  std::vector<PdfObjectPtr> items;
};
struct PdfDict
{
  std::map<std::string, PdfObjectPtr> entries;

  const PdfObject* Find(std::string_view key) const
  {
    const auto it = entries.find(std::string(key));
    return it == entries.end() ? nullptr : it->second.get();
  }
};

struct PdfObject
{
  using Value = std::variant<std::nullptr_t,
                             bool,
                             int64_t,
                             double,
                             PdfName,
                             PdfString,
                             PdfArray,
                             std::shared_ptr<PdfDict>,
                             PdfRef>;
  Value value;

  PdfObject() = default;
  explicit PdfObject(Value v) : value(std::move(v)) {}
};

template <typename T> const T* GetIf(const PdfObject& obj)
{
  return std::get_if<T>(&obj.value);
}

// ---------------------------------------------------------------------------
// Lexer helpers
// ---------------------------------------------------------------------------

bool IsWs(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0';
}

void SkipWsAndComments(std::string_view s, size_t& pos)
{
  while (pos < s.size()) {
    if (IsWs(s[pos])) {
      ++pos;
    } else if (s[pos] == '%') {
      while (pos < s.size() && s[pos] != '\n' && s[pos] != '\r')
        ++pos;
    } else {
      break;
    }
  }
}

bool ParseInt(std::string_view s, int64_t* out)
{
  if (s.empty())
    return false;
  size_t i = 0;
  bool neg = false;
  if (s[0] == '+' || s[0] == '-') {
    neg = s[0] == '-';
    i = 1;
  }
  if (i >= s.size())
    return false;
  int64_t v = 0;
  for (; i < s.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(s[i])))
      return false;
    v = v * 10 + (s[i] - '0');
  }
  *out = neg ? -v : v;
  return true;
}

std::optional<int64_t> ReadNumberToken(std::string_view s, size_t& pos)
{
  SkipWsAndComments(s, pos);
  const size_t start = pos;
  if (pos < s.size() && (s[pos] == '+' || s[pos] == '-'))
    ++pos;
  const size_t digits_start = pos;
  while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])))
    ++pos;
  if (pos == digits_start) {
    pos = start;
    return std::nullopt;
  }
  int64_t v = 0;
  if (!ParseInt(s.substr(start, pos - start), &v)) {
    pos = start;
    return std::nullopt;
  }
  return v;
}

// Decodes a PDF name: strips the leading '/', resolves #XX escapes.
std::string ParseNameToken(std::string_view s, size_t& pos)
{
  ++pos; // skip '/'
  std::string out;
  auto hexv = [](char h) -> int {
    if (h >= '0' && h <= '9')
      return h - '0';
    if (h >= 'a' && h <= 'f')
      return h - 'a' + 10;
    if (h >= 'A' && h <= 'F')
      return h - 'A' + 10;
    return -1;
  };
  while (pos < s.size()) {
    const char c = s[pos];
    if (IsWs(c) || c == '/' || c == '[' || c == ']' || c == '<' || c == '>' || c == '(' ||
        c == ')' || c == '{' || c == '}') {
      break;
    }
    if (c == '#' && pos + 2 < s.size() && hexv(s[pos + 1]) >= 0 && hexv(s[pos + 2]) >= 0) {
      out.push_back(static_cast<char>((hexv(s[pos + 1]) << 4) | hexv(s[pos + 2])));
      pos += 3;
      continue;
    }
    out.push_back(c);
    ++pos;
  }
  return out;
}

// Parses a literal string ( ... ) including escapes, or hex string < ... >.
std::string ParseStringToken(std::string_view s, size_t& pos)
{
  const char open = s[pos];
  ++pos;
  if (open == '<') {
    std::string out;
    auto hexv = [](char h) -> int {
      if (h >= '0' && h <= '9')
        return h - '0';
      if (h >= 'a' && h <= 'f')
        return h - 'a' + 10;
      if (h >= 'A' && h <= 'F')
        return h - 'A' + 10;
      return -1;
    };
    while (pos < s.size() && s[pos] != '>') {
      if (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n') {
        ++pos;
        continue;
      }
      const int hi = hexv(s[pos]);
      const int lo = pos + 1 < s.size() ? hexv(s[pos + 1]) : 0;
      if (hi < 0) {
        ++pos;
        continue;
      }
      out.push_back(static_cast<char>((hi << 4) | (lo < 0 ? 0 : lo)));
      pos += 2;
    }
    if (pos < s.size())
      ++pos;
    return out;
  }
  std::string out;
  int depth = 0;
  while (pos < s.size()) {
    const char c = s[pos];
    if (c == '\\' && pos + 1 < s.size()) {
      const char e = s[pos + 1];
      switch (e) {
      case 'n':
        out.push_back('\n');
        pos += 2;
        break;
      case 'r':
        out.push_back('\r');
        pos += 2;
        break;
      case 't':
        out.push_back('\t');
        pos += 2;
        break;
      case 'b':
        out.push_back('\b');
        pos += 2;
        break;
      case 'f':
        out.push_back('\f');
        pos += 2;
        break;
      case '(':
        out.push_back('(');
        pos += 2;
        break;
      case ')':
        out.push_back(')');
        pos += 2;
        break;
      case '\\':
        out.push_back('\\');
        pos += 2;
        break;
      default:
        if (e >= '0' && e <= '7') {
          int v = 0;
          size_t i = pos + 1;
          int n = 0;
          while (i < s.size() && n < 3 && s[i] >= '0' && s[i] <= '7') {
            v = v * 8 + (s[i] - '0');
            ++i;
            ++n;
          }
          out.push_back(static_cast<char>(v));
          pos = i;
        } else {
          out.push_back(e);
          pos += 2;
        }
      }
      continue;
    }
    if (c == '(') {
      ++depth;
      out.push_back(c);
      ++pos;
      continue;
    }
    if (c == ')') {
      if (depth == 0) {
        ++pos;
        break;
      }
      --depth;
      out.push_back(c);
      ++pos;
      continue;
    }
    out.push_back(c);
    ++pos;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Object parser
// ---------------------------------------------------------------------------

PdfObjectPtr ParseObjectPtr(std::string_view s, size_t& pos, int depth);

PdfDict ParseDict(std::string_view s, size_t& pos, int depth)
{
  PdfDict dict; // pos at "<<"
  pos += 2;
  for (;;) {
    SkipWsAndComments(s, pos);
    if (pos < s.size() && s[pos] == '>' && pos + 1 < s.size() && s[pos + 1] == '>') {
      pos += 2;
      break;
    }
    if (pos >= s.size() || s[pos] != '/')
      break;
    const std::string key = ParseNameToken(s, pos);
    if (key.empty())
      break;
    dict.entries[std::move(key)] = ParseObjectPtr(s, pos, depth + 1);
  }
  return dict;
}

PdfObjectPtr ParseObjectPtr(std::string_view s, size_t& pos, int depth)
{
  if (depth > 64)
    return std::make_shared<PdfObject>(nullptr);
  SkipWsAndComments(s, pos);
  if (pos >= s.size())
    return std::make_shared<PdfObject>(nullptr);

  const char c = s[pos];
  if (c == '[') {
    PdfArray array;
    ++pos;
    for (;;) {
      SkipWsAndComments(s, pos);
      if (pos >= s.size())
        break;
      if (s[pos] == ']') {
        ++pos;
        break;
      }
      array.items.push_back(ParseObjectPtr(s, pos, depth + 1));
    }
    return std::make_shared<PdfObject>(std::move(array));
  }
  if (c == '<' && pos + 1 < s.size() && s[pos + 1] == '<') {
    return std::make_shared<PdfObject>(std::make_shared<PdfDict>(ParseDict(s, pos, depth)));
  }
  if (c == '<')
    return std::make_shared<PdfObject>(PdfString{ParseStringToken(s, pos)});
  if (c == '(')
    return std::make_shared<PdfObject>(PdfString{ParseStringToken(s, pos)});
  if (c == '/')
    return std::make_shared<PdfObject>(PdfName{ParseNameToken(s, pos)});
  if (s.substr(pos, 4) == "true") {
    pos += 4;
    return std::make_shared<PdfObject>(true);
  }
  if (s.substr(pos, 5) == "false") {
    pos += 5;
    return std::make_shared<PdfObject>(false);
  }
  if (s.substr(pos, 4) == "null") {
    pos += 4;
    return std::make_shared<PdfObject>(nullptr);
  }

  // Number, possibly an indirect reference "N G R".  Handles integers and
  // real numbers with a decimal fraction (PDF 1.7 §7.3.3).
  const size_t num_start = pos;
  if (pos < s.size() && (s[pos] == '+' || s[pos] == '-'))
    ++pos;
  size_t digits = 0;
  while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
    ++pos;
    ++digits;
  }
  bool is_real = false;
  if (pos < s.size() && s[pos] == '.') {
    is_real = true;
    ++pos;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
      ++pos;
      ++digits;
    }
  }
  if (digits == 0) {
    // Guarantee forward progress: callers (e.g. the array loop) may retry
    // on the same position, which would otherwise spin forever on bytes
    // that are not valid PDF tokens (binary inline-image data etc.).
    if (pos < s.size())
      ++pos;
    return std::make_shared<PdfObject>(nullptr);
  }
  if (is_real) {
    const std::string token(s.substr(num_start, pos - num_start));
    char* end = nullptr;
    const double v = std::strtod(token.c_str(), &end);
    if (end == token.c_str())
      return std::make_shared<PdfObject>(nullptr);
    return std::make_shared<PdfObject>(v);
  }
  int64_t first = 0;
  if (!ParseInt(s.substr(num_start, pos - num_start), &first)) {
    return std::make_shared<PdfObject>(nullptr);
  }
  const size_t after_first = pos;

  const auto gen = ReadNumberToken(s, pos);
  if (!gen.has_value()) {
    pos = after_first;
    return std::make_shared<PdfObject>(first);
  }
  SkipWsAndComments(s, pos);
  if (pos < s.size() && s[pos] == 'R' &&
      (pos + 1 == s.size() || IsWs(s[pos + 1]) || s[pos + 1] == '/' || s[pos + 1] == '[' ||
       s[pos + 1] == '<' || s[pos + 1] == '(' || s[pos + 1] == ']')) {
    ++pos;
    return std::make_shared<PdfObject>(PdfRef{first, gen.value()});
  }
  // Not a reference: rewind so the "gen" token is left for the caller to
  // parse as a separate value (e.g. "[0 0 612 792]").
  pos = after_first;
  return std::make_shared<PdfObject>(first);
}

// ---------------------------------------------------------------------------
// Inflate helpers
// ---------------------------------------------------------------------------

base::Result<std::string> InflateZlib(std::string_view in)
{
  z_stream zs{};
  if (inflateInit(&zs) != Z_OK) {
    return base::Error::Parse("pdf: inflateInit failed");
  }
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  zs.avail_in = static_cast<uInt>(in.size());
  std::string out;
  char buf[16384];
  int ret = Z_OK;
  while (ret != Z_STREAM_END) {
    zs.next_out = reinterpret_cast<Bytef*>(buf);
    zs.avail_out = sizeof(buf);
    ret = inflate(&zs, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      inflateEnd(&zs);
      return base::Error::Parse("pdf: corrupt deflate stream");
    }
    out.append(buf, sizeof(buf) - zs.avail_out);
    if (zs.avail_in == 0 && ret != Z_STREAM_END && zs.avail_out == sizeof(buf)) {
      inflateEnd(&zs);
      return base::Error::Parse("pdf: truncated deflate stream");
    }
  }
  inflateEnd(&zs);
  return out;
}

// Applies a stream /DecodeParms predictor (TIFF type 2 or PNG types 10-15).
bool ApplyPredictor(int predictor, int colors, int bpc, int columns, std::string& data)
{
  if (predictor == 1)
    return true;
  const int bpp_int = std::max(1, (colors * bpc + 7) / 8);
  const size_t bpp = static_cast<size_t>(bpp_int);
  const size_t row_bytes = static_cast<size_t>(columns) * bpp;
  if (predictor == 2) {
    for (size_t row_start = 0; row_start < data.size(); row_start += row_bytes) {
      for (size_t i = bpp; i < row_bytes && row_start + i < data.size(); ++i) {
        data[row_start + i] = static_cast<char>(static_cast<uint8_t>(data[row_start + i]) +
                                                static_cast<uint8_t>(data[row_start + i - bpp]));
      }
    }
    return true;
  }
  if (predictor >= 10 && predictor <= 15) {
    std::string decoded;
    decoded.reserve(data.size());
    std::vector<uint8_t> prev_row(row_bytes, 0);
    size_t pos = 0;
    while (pos < data.size()) {
      const uint8_t filter = static_cast<uint8_t>(data[pos++]);
      if (pos + row_bytes > data.size())
        break;
      std::vector<uint8_t> row(row_bytes);
      for (size_t i = 0; i < row_bytes; ++i) {
        const uint8_t raw = static_cast<uint8_t>(data[pos + i]);
        const uint8_t a = i >= bpp ? row[i - bpp] : 0;
        const uint8_t b = prev_row[i];
        const uint8_t c = i >= bpp ? prev_row[i - bpp] : 0;
        int v = 0;
        switch (filter) {
        case 0:
          v = raw;
          break;
        case 1:
          v = raw + a;
          break;
        case 2:
          v = raw + b;
          break;
        case 3:
          v = raw + (a + b) / 2;
          break;
        case 4: {
          const int p = a + b - c;
          const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
          const int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          v = raw + pr;
          break;
        }
        default:
          return false;
        }
        row[i] = static_cast<uint8_t>(v & 0xFF);
      }
      decoded.append(reinterpret_cast<const char*>(row.data()), row_bytes);
      prev_row = std::move(row);
      pos += row_bytes;
    }
    data = std::move(decoded);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Content-stream tokenizer
// ---------------------------------------------------------------------------

struct ContentToken
{
  bool is_operator = false;
  PdfObjectPtr operand; // valid when !is_operator
  std::string op;       // valid when is_operator
};

std::vector<ContentToken> TokenizeContent(std::string_view content)
{
  std::vector<ContentToken> tokens;
  size_t pos = 0;
  while (pos < content.size()) {
    SkipWsAndComments(content, pos);
    if (pos >= content.size())
      break;
    const char c = content[pos];

    if (c == '/' || c == '(' || c == '[' || c == '<') {
      tokens.push_back(ContentToken{false, ParseObjectPtr(content, pos, 0), {}});
      continue;
    }
    if (c == ']' || c == '>') {
      ++pos;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.') {
      tokens.push_back(ContentToken{false, ParseObjectPtr(content, pos, 0), {}});
      continue;
    }
    const size_t start = pos;
    while (pos < content.size()) {
      const char k = content[pos];
      if (IsWs(k) || k == '/' || k == '<' || k == '>' || k == '[' || k == ']' || k == '(' ||
          k == ')' || k == '{' || k == '}') {
        break;
      }
      ++pos;
    }
    tokens.push_back(ContentToken{true, nullptr, std::string(content.substr(start, pos - start))});
  }
  return tokens;
}

// ---------------------------------------------------------------------------
// Text extraction (token stream -> text)
// ---------------------------------------------------------------------------

std::string DecodeTextBytes(std::string_view bytes)
{
  if (bytes.size() >= 2 && static_cast<uint8_t>(bytes[0]) == 0xFE &&
      static_cast<uint8_t>(bytes[1]) == 0xFF) {
    // UTF-16BE.
    std::string out;
    for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
      const char32_t cp = static_cast<char32_t>((static_cast<uint32_t>(bytes[i]) << 8) |
                                                static_cast<uint32_t>(bytes[i + 1]));
      out += base::EncodeUtf8(cp);
    }
    return out;
  }
  // Best-effort: ASCII + Latin-1 approximation for high bytes.
  std::string out;
  for (const char raw : bytes) {
    const unsigned char c = static_cast<unsigned char>(raw);
    if (c == 0x0D || c == 0x0A) {
      out.push_back('\n');
    } else if (c < 0x20) {
      // skip control bytes
    } else if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else {
      out += base::EncodeUtf8(static_cast<char32_t>(c));
    }
  }
  return out;
}

void AppendText(std::string& out, std::string_view bytes, bool& line_start)
{
  const std::string decoded = DecodeTextBytes(bytes);
  if (decoded.empty())
    return;
  if (!out.empty() && !line_start && out.back() != '\n' && out.back() != ' ') {
    out.push_back(' ');
  }
  out += decoded;
  line_start = false;
}

std::string ExtractTextFromContent(std::string_view content)
{
  const auto tokens = TokenizeContent(content);
  std::string out;
  bool line_start = true;
  std::vector<PdfObjectPtr> operands;

  auto handle_line_break = [&] {
    if (!out.empty() && out.back() != '\n')
      out.push_back('\n');
    line_start = true;
  };

  for (const ContentToken& token : tokens) {
    if (!token.is_operator) {
      operands.push_back(token.operand);
      continue;
    }
    const std::string_view op = token.op;

    if (op == "BT") {
      line_start = true;
    } else if (op == "ET") {
      // end of a text object; nothing to flush for extraction
    } else if (op == "Tj" && !operands.empty()) {
      if (const auto* s = GetIf<PdfString>(*operands.back())) {
        AppendText(out, s->value, line_start);
      }
    } else if (op == "'" && !operands.empty()) {
      if (const auto* s = GetIf<PdfString>(*operands.back())) {
        handle_line_break();
        AppendText(out, s->value, line_start);
      }
    } else if (op == "\"" && !operands.empty()) {
      // operands: (tw, tc, string); the string is the last one.
      if (const auto* s = GetIf<PdfString>(*operands.back())) {
        handle_line_break();
        AppendText(out, s->value, line_start);
      }
    } else if (op == "TJ" && !operands.empty()) {
      if (const auto* arr = GetIf<PdfArray>(*operands.back())) {
        for (const PdfObjectPtr& item : arr->items) {
          if (const auto* s = GetIf<PdfString>(*item)) {
            AppendText(out, s->value, line_start);
          }
        }
      }
    } else if (op == "Td" || op == "TD" || op == "T*") {
      handle_line_break();
    } else if (op == "Tm") {
      // New text matrix: approximate as a new line.
      handle_line_break();
    }
    // All other operators are ignored for extraction.

    operands.clear();
  }

  while (!out.empty() && out.back() == '\n')
    out.pop_back();
  return out;
}

// ---------------------------------------------------------------------------
// Parser state
// ---------------------------------------------------------------------------

// Simple 2D affine transform in PDF form [a b c d e f]: x' = a*x + c*y + e,
// y' = b*x + d*y + f.
struct PdfMatrix
{
  float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

  PdfMatrix Mul(const PdfMatrix& o) const
  {
    PdfMatrix m;
    m.a = a * o.a + c * o.b;
    m.b = b * o.a + d * o.b;
    m.c = a * o.c + c * o.d;
    m.d = b * o.c + d * o.d;
    m.e = a * o.e + c * o.f + e;
    m.f = b * o.e + d * o.f + f;
    return m;
  }
};

struct PdfPt
{
  float x = 0, y = 0;
};

// Font metrics taken from a PDF font dictionary: /Widths (in 1/1000 em) and
// /FirstChar.  An empty widths vector means "unknown" (0.5 em approximation).
struct PdfFontMetrics
{
  std::vector<float> widths;
  int first_char = 0;
};

// ---------------------------------------------------------------------------
// Page renderer: content stream → RGBA image.
//
// Supports the core graphics operators (m/l/c/v/y/h/re/f/f*/S/s/B/b*/n,
// q/Q/cm/w/RG/rg/g/G/k/K) and text operators (BT/ET, Tf, Td/TD/Tm/T*, TL,
// Tj/TJ/'/\", Tc/Tw/Ts/Tz/Tr).  Text uses the engine's FreeType stack
// (sans-serif) with /Widths-based advances when available; glyphs are drawn
// unrotated at the transform's x-scale (documented approximation).  Image
// operators (BI/ID/EI/Do) and clipping are not implemented: their tokens are
// skipped so the rest of the page still renders.
// ---------------------------------------------------------------------------
class PdfPageRenderer
{
public:
  PdfPageRenderer(const std::map<std::string, PdfFontMetrics>& fonts,
                  int width,
                  int height,
                  float scale,
                  const graphics::FontRegistry* registry)
      : fonts_(fonts), width_(width), height_(height), scale_(scale), registry_(registry)
  {}

  base::Result<image::Image> Run(std::string_view content)
  {
    img_.width = width_;
    img_.height = height_;
    img_.rgba.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4, 255);
    if (registry_ != nullptr) {
      text_font_ = registry_->SelectorFor("sans-serif");
    }
    const std::vector<ContentToken> tokens = TokenizeContent(content);
    for (const ContentToken& token : tokens) {
      if (token.is_operator) {
        Exec(token.op);
      } else {
        operands_.push_back(token.operand);
      }
    }
    return img_;
  }

private:
  struct GState
  {
    PdfMatrix ctm;
    float fill_r = 0, fill_g = 0, fill_b = 0; // initial fill = black
    float stroke_r = 0, stroke_g = 0, stroke_b = 0;
    float line_width = 1;
    // Text state.
    float font_size = 12;
    const PdfFontMetrics* font = nullptr;
    float char_space = 0;
    float word_space = 0;
    float leading = 0;
    float text_rise = 0;
    int render_mode = 0;
    PdfMatrix text_matrix;      // current text matrix (Tm / Td accumulate)
    PdfMatrix text_line_matrix; // line-start text matrix (for T*)
  };

  const std::map<std::string, PdfFontMetrics>& fonts_;
  int width_ = 0;
  int height_ = 0;
  float scale_ = 1;
  const graphics::FontRegistry* registry_ = nullptr;
  const graphics::FontSelector* text_font_ = nullptr;
  image::Image img_;
  GState state_;
  std::vector<GState> stack_;
  std::vector<PdfObjectPtr> operands_;

  // Current path (points in PDF user space, before transform).
  struct SubPath
  {
    std::vector<PdfPt> points;
    bool closed = false;
  };
  std::vector<SubPath> path_;
  PdfPt current_{0, 0};
  PdfPt path_start_{0, 0};
  bool has_current_ = false;

  // -------------------------------------------------------------------------
  // Number helpers
  // -------------------------------------------------------------------------
  static float ToFloat(const PdfObject& obj, float def)
  {
    if (const auto* v = GetIf<double>(obj)) {
      return static_cast<float>(*v);
    }
    if (const auto* v = GetIf<int64_t>(obj)) {
      return static_cast<float>(*v);
    }
    return def;
  }

  bool PopNums(int n, std::vector<float>& out)
  {
    out.clear();
    if (static_cast<int>(operands_.size()) < n) {
      return false;
    }
    for (int i = 0; i < n; ++i) {
      out.push_back(ToFloat(
          *operands_[operands_.size() - static_cast<std::size_t>(n) + static_cast<std::size_t>(i)],
          0.0f));
    }
    operands_.resize(operands_.size() - static_cast<std::size_t>(n));
    return true;
  }

  // -------------------------------------------------------------------------
  // Pixel helpers (screen space: y grows downward).
  // -------------------------------------------------------------------------
  void SetPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
  {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
      return;
    }
    const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                           static_cast<std::size_t>(x)) *
                          4;
    img_.rgba[o + 0] = r;
    img_.rgba[o + 1] = g;
    img_.rgba[o + 2] = b;
    img_.rgba[o + 3] = 255;
  }

  // Alpha-blends a glyph pixel (premultiplied gray) over the buffer.
  void BlendGlyphPixel(int x, int y, uint8_t alpha, uint8_t r, uint8_t g, uint8_t b)
  {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
      return;
    }
    const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                           static_cast<std::size_t>(x)) *
                          4;
    const int a = alpha;
    const int ia = 255 - a;
    img_.rgba[o + 0] = static_cast<uint8_t>((r * a + img_.rgba[o + 0] * ia) / 255);
    img_.rgba[o + 1] = static_cast<uint8_t>((g * a + img_.rgba[o + 1] * ia) / 255);
    img_.rgba[o + 2] = static_cast<uint8_t>((b * a + img_.rgba[o + 2] * ia) / 255);
    img_.rgba[o + 3] = 255;
  }

  // Transforms a PDF-space point to pixel space.
  PdfPt ToPixel(float x, float y) const
  {
    const PdfMatrix& m = state_.ctm;
    const float tx = m.a * x + m.c * y + m.e;
    const float ty = m.b * x + m.d * y + m.f;
    return PdfPt{tx * scale_, static_cast<float>(height_) - ty * scale_};
  }

  // -------------------------------------------------------------------------
  // Path construction
  // -------------------------------------------------------------------------
  void BeginPath()
  {
    path_.clear();
    has_current_ = false;
  }

  void MoveTo(float x, float y)
  {
    if (!path_.empty() && !path_.back().closed) {
      // A move starts a new sub-path (previous stays open).
    }
    path_.push_back(SubPath{});
    path_.back().points.push_back(PdfPt{x, y});
    current_ = PdfPt{x, y};
    path_start_ = current_;
    has_current_ = true;
  }

  void LineTo(float x, float y)
  {
    if (!has_current_) {
      MoveTo(x, y);
      return;
    }
    if (!path_.empty()) {
      path_.back().points.push_back(PdfPt{x, y});
    }
    current_ = PdfPt{x, y};
  }

  // De Casteljau flattening of a cubic bezier; appends to the current sub-path.
  void CurveTo(float x1, float y1, float x2, float y2, float x3, float y3, int depth = 0)
  {
    if (!has_current_) {
      MoveTo(0, 0);
    }
    // Flatness test: distance of the control points from the chord.
    const float dx = x3 - current_.x;
    const float dy = y3 - current_.y;
    const float d1 = std::fabs((x1 - current_.x) * dy - (y1 - current_.y) * dx);
    const float d2 = std::fabs((x2 - current_.x) * dy - (y2 - current_.y) * dx);
    const float chord2 = dx * dx + dy * dy;
    if (depth >= 14 || (chord2 > 0 && (d1 + d2) * (d1 + d2) <= 0.25f * chord2)) {
      LineTo(x3, y3);
      return;
    }
    // Subdivide.
    const float ax = (current_.x + x1) / 2, ay = (current_.y + y1) / 2;
    const float bx = (x1 + x2) / 2, by = (y1 + y2) / 2;
    const float cx = (x2 + x3) / 2, cy = (y2 + y3) / 2;
    const float dx2 = (ax + bx) / 2, dy2 = (ay + by) / 2;
    const float ex = (bx + cx) / 2, ey = (by + cy) / 2;
    const float mx = (dx2 + ex) / 2, my = (dy2 + ey) / 2;
    CurveTo(ax, ay, dx2, dy2, mx, my, depth + 1);
    CurveTo(ex, ey, cx, cy, x3, y3, depth + 1);
  }

  // -------------------------------------------------------------------------
  // Fill / stroke (pixel-space scan conversion)
  // -------------------------------------------------------------------------
  // Gathers the transformed polygons for every sub-path (always closed: the
  // first vertex is appended so the closing edge is included).
  std::vector<std::vector<PdfPt>> PixelPolygons() const
  {
    std::vector<std::vector<PdfPt>> out;
    for (const SubPath& sub : path_) {
      if (sub.points.size() < 2) {
        continue;
      }
      std::vector<PdfPt> poly;
      poly.reserve(sub.points.size() + 1);
      for (const PdfPt& p : sub.points) {
        poly.push_back(ToPixel(p.x, p.y));
      }
      poly.push_back(poly.front());
      out.push_back(std::move(poly));
    }
    return out;
  }

  void FillPath(bool even_odd)
  {
    const std::vector<std::vector<PdfPt>> polys = PixelPolygons();
    if (polys.empty()) {
      return;
    }
    const uint8_t r = static_cast<uint8_t>(std::clamp(state_.fill_r, 0.0f, 1.0f) * 255.0f);
    const uint8_t g = static_cast<uint8_t>(std::clamp(state_.fill_g, 0.0f, 1.0f) * 255.0f);
    const uint8_t b = static_cast<uint8_t>(std::clamp(state_.fill_b, 0.0f, 1.0f) * 255.0f);
    float min_y = polys[0][0].y, max_y = polys[0][0].y;
    for (const std::vector<PdfPt>& poly : polys) {
      for (const PdfPt& p : poly) {
        min_y = std::min(min_y, p.y);
        max_y = std::max(max_y, p.y);
      }
    }
    // Scan-line fill over ALL sub-paths together so even-odd and winding rules
    // see the complete path (a hole inside a filled region cancels out).
    // The scan range is clamped to the canvas; PDFs may contain paths with
    // far-out-of-page coordinates (e.g. huge clip rectangles).
    const int y0 = std::max(-1, static_cast<int>(std::floor(min_y)));
    const int y1 = std::min(height_, static_cast<int>(std::floor(max_y)));
    for (int y = y0; y <= y1; ++y) {
      const float scan = static_cast<float>(y) + 0.5f;
      std::vector<std::pair<float, int>> xs;
      for (const std::vector<PdfPt>& poly : polys) {
        for (std::size_t i = 0; i + 1 < poly.size(); ++i) {
          const PdfPt& p0 = poly[i];
          const PdfPt& p1 = poly[i + 1];
          const bool c0 = p0.y <= scan, c1 = p1.y <= scan;
          if (c0 == c1) {
            continue;
          }
          const float x = p0.x + (scan - p0.y) * (p1.x - p0.x) / (p1.y - p0.y);
          xs.push_back({x, p1.y > p0.y ? 1 : -1});
        }
      }
      std::sort(xs.begin(), xs.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
      });
      if (even_odd) {
        for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
          FillSpan(xs[i].first, xs[i + 1].first, y, r, g, b);
        }
      } else {
        int winding = 0;
        float start = 0;
        for (const auto& [x, dir] : xs) {
          if (winding == 0) {
            start = x;
          }
          winding += dir;
          if (winding == 0) {
            FillSpan(start, x, y, r, g, b);
          }
        }
      }
    }
  }

  void FillSpan(float x0, float x1, int y, uint8_t r, uint8_t g, uint8_t b)
  {
    if (x1 < x0) {
      std::swap(x0, x1);
    }
    const int start = std::max(0, static_cast<int>(std::floor(x0)));
    const int end = std::min(width_, static_cast<int>(std::ceil(x1)));
    for (int x = start; x < end; ++x) {
      SetPixel(x, y, r, g, b);
    }
  }

  void StrokePath()
  {
    const std::vector<std::vector<PdfPt>> polys = PixelPolygons();
    if (polys.empty()) {
      return;
    }
    const uint8_t r = static_cast<uint8_t>(std::clamp(state_.stroke_r, 0.0f, 1.0f) * 255.0f);
    const uint8_t g = static_cast<uint8_t>(std::clamp(state_.stroke_g, 0.0f, 1.0f) * 255.0f);
    const uint8_t b = static_cast<uint8_t>(std::clamp(state_.stroke_b, 0.0f, 1.0f) * 255.0f);
    // PDF: a 0 line width renders as the thinnest line (1 device pixel).
    const int half = std::max(1, static_cast<int>(state_.line_width * scale_ / 2.0f));
    const int size = half * 2 + 1;
    const float step = std::max(0.5f, static_cast<float>(half));
    for (const std::vector<PdfPt>& poly : polys) {
      for (std::size_t i = 0; i + 1 < poly.size(); ++i) {
        const PdfPt& p0 = poly[i];
        const PdfPt& p1 = poly[i + 1];
        const float dx = p1.x - p0.x;
        const float dy = p1.y - p0.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0) {
          continue;
        }
        // Segments far outside the canvas would otherwise generate an
        // enormous number of (clamped) steps; cap the work to the diagonal.
        const int steps =
            std::max(1, std::min(static_cast<int>(len / step), 2 * (width_ + height_) + 16));
        for (int s = 0; s <= steps; ++s) {
          const float t = static_cast<float>(s) / static_cast<float>(steps);
          const int cx = static_cast<int>(std::lround(p0.x + dx * t));
          const int cy = static_cast<int>(std::lround(p0.y + dy * t));
          for (int y = cy - half; y <= cy + half; ++y) {
            for (int x = cx - half; x <= cx + half; ++x) {
              SetPixel(x, y, r, g, b);
            }
          }
        }
        (void)size;
      }
    }
  }

  void RectToPath(float x, float y, float w, float h)
  {
    // The re operator appends a complete sub-path to the CURRENT path
    // (PDF 1.7 §8.5.3.2); it must not clear previously built sub-paths.
    MoveTo(x, y);
    LineTo(x + w, y);
    LineTo(x + w, y + h);
    LineTo(x, y + h);
    path_.back().closed = true;
    current_ = PdfPt{x, y};
    has_current_ = true;
  }

  // -------------------------------------------------------------------------
  // Text
  // -------------------------------------------------------------------------
  // Decodes a simple-font byte string into code points (Latin-1 identity;
  // matches PDF's simple-font encodings for the ASCII + Latin-1 range).
  static std::vector<uint32_t> DecodeSimple(std::string_view bytes)
  {
    std::vector<uint32_t> out;
    out.reserve(bytes.size());
    for (const char byte : bytes) {
      out.push_back(static_cast<uint32_t>(static_cast<unsigned char>(byte)));
    }
    return out;
  }

  void ShowText(std::string_view bytes)
  {
    const float fs = state_.font_size;
    // Advance for a code point in PDF text units (glyph units are 1/1000 em).
    const auto advance_for = [&](uint32_t cp) -> float {
      if (state_.font != nullptr && !state_.font->widths.empty()) {
        const int code = static_cast<int>(cp) - state_.font->first_char;
        if (code >= 0 && code < static_cast<int>(state_.font->widths.size())) {
          return state_.font->widths[static_cast<std::size_t>(code)] * 0.001f * fs;
        }
      }
      return 0.5f * fs; // documented approximation (no font metrics available)
    };
    if (state_.render_mode == 3) {
      // Invisible text: advance without drawing.
      float pen = 0;
      for (const uint32_t cp : DecodeSimple(bytes)) {
        pen += advance_for(cp);
      }
      state_.text_matrix.e += pen * std::max(std::fabs(state_.text_matrix.a), 0.0001f);
      return;
    }
    const uint8_t r = static_cast<uint8_t>(std::clamp(state_.fill_r, 0.0f, 1.0f) * 255.0f);
    const uint8_t g = static_cast<uint8_t>(std::clamp(state_.fill_g, 0.0f, 1.0f) * 255.0f);
    const uint8_t b = static_cast<uint8_t>(std::clamp(state_.fill_b, 0.0f, 1.0f) * 255.0f);
    // The glyph size is the font size scaled by the CTM's x-axis stretch (the
    // text matrix's own scale multiplies advances, not the glyph outlines).
    const float ctm_xscale = std::hypot(state_.ctm.a, state_.ctm.b);
    const float px_size =
        std::min(fs * scale_ * std::max(ctm_xscale, 0.01f), 4.0f * static_cast<float>(height_));
    const PdfMatrix tm = state_.text_matrix.Mul(state_.ctm);
    for (const uint32_t cp : DecodeSimple(bytes)) {
      if (cp == 0x20) {
        state_.text_matrix.e += (advance_for(cp) + state_.word_space) *
                                std::max(std::fabs(state_.text_matrix.a), 0.0001f);
        continue;
      }
      if (cp < 0x20) {
        continue; // control bytes: skip
      }
      // Pen position in pixel space.
      const float bx = tm.e;
      const float by = tm.f;
      const int pen_x = static_cast<int>(std::lround(bx * scale_));
      const int pen_y = static_cast<int>(std::lround(static_cast<float>(height_) - by * scale_));
      if (registry_ != nullptr && text_font_ != nullptr) {
        const auto glyph = text_font_->RenderGlyph(cp, px_size);
        if (glyph.has_value()) {
          const graphics::GlyphBitmap& gb = glyph.value().glyph;
          for (int gy = 0; gy < gb.height; ++gy) {
            const int dest_y = pen_y - gb.top + gy;
            if (dest_y < 0 || dest_y >= height_) {
              continue;
            }
            for (int gx = 0; gx < gb.width; ++gx) {
              const uint8_t alpha =
                  gb.data[static_cast<std::size_t>(gy) * static_cast<std::size_t>(gb.pitch) +
                          static_cast<std::size_t>(gx)];
              if (alpha != 0) {
                BlendGlyphPixel(pen_x + gb.left + gx, dest_y, alpha, r, g, b);
              }
            }
          }
        }
      }
      state_.text_matrix.e += (advance_for(cp) + state_.char_space) *
                              std::max(std::fabs(state_.text_matrix.a), 0.0001f);
    }
  }

  // -------------------------------------------------------------------------
  // Operator dispatch
  // -------------------------------------------------------------------------
  void Exec(const std::string& op)
  {
    std::vector<float> v;
    if (op == "q") {
      stack_.push_back(state_);
    } else if (op == "Q") {
      if (!stack_.empty()) {
        state_ = stack_.back();
        stack_.pop_back();
      }
    } else if (op == "cm") {
      if (PopNums(6, v)) {
        state_.ctm = state_.ctm.Mul(PdfMatrix{v[0], v[1], v[2], v[3], v[4], v[5]});
      }
    } else if (op == "m") {
      if (PopNums(2, v)) {
        MoveTo(v[0], v[1]);
      }
    } else if (op == "l") {
      if (PopNums(2, v)) {
        LineTo(v[0], v[1]);
      }
    } else if (op == "c") {
      if (PopNums(6, v)) {
        CurveTo(v[0], v[1], v[2], v[3], v[4], v[5]);
      }
    } else if (op == "v") {
      if (PopNums(4, v)) {
        CurveTo(current_.x, current_.y, v[0], v[1], v[2], v[3]);
      }
    } else if (op == "y") {
      if (PopNums(4, v)) {
        CurveTo(v[0], v[1], v[2], v[3], v[2], v[3]);
      }
    } else if (op == "h") {
      if (has_current_ && !path_.empty()) {
        path_.back().points.push_back(path_start_);
        path_.back().closed = true;
        current_ = path_start_;
      }
    } else if (op == "re") {
      if (PopNums(4, v)) {
        RectToPath(v[0], v[1], v[2], v[3]);
      }
    } else if (op == "f" || op == "F") {
      FillPath(/*even_odd=*/false);
      BeginPath();
    } else if (op == "f*") {
      FillPath(/*even_odd=*/true);
      BeginPath();
    } else if (op == "S") {
      StrokePath();
      BeginPath();
    } else if (op == "s") {
      if (!path_.empty()) {
        CloseLast();
      }
      StrokePath();
      BeginPath();
    } else if (op == "B") {
      FillPath(false);
      StrokePath();
      BeginPath();
    } else if (op == "B*") {
      FillPath(true);
      StrokePath();
      BeginPath();
    } else if (op == "b") {
      CloseLast();
      FillPath(false);
      StrokePath();
      BeginPath();
    } else if (op == "b*") {
      CloseLast();
      FillPath(true);
      StrokePath();
      BeginPath();
    } else if (op == "n") {
      BeginPath();
    } else if (op == "w") {
      if (PopNums(1, v)) {
        state_.line_width = std::max(0.0f, v[0]);
      }
    } else if (op == "RG") {
      if (PopNums(3, v)) {
        state_.stroke_r = v[0];
        state_.stroke_g = v[1];
        state_.stroke_b = v[2];
      }
    } else if (op == "rg") {
      if (PopNums(3, v)) {
        state_.fill_r = v[0];
        state_.fill_g = v[1];
        state_.fill_b = v[2];
      }
    } else if (op == "G") {
      if (PopNums(1, v)) {
        state_.stroke_r = state_.stroke_g = state_.stroke_b = v[0];
      }
    } else if (op == "g") {
      if (PopNums(1, v)) {
        state_.fill_r = state_.fill_g = state_.fill_b = v[0];
      }
    } else if (op == "K") {
      if (PopNums(4, v)) {
        CmykToRgb(v[0], v[1], v[2], v[3], state_.stroke_r, state_.stroke_g, state_.stroke_b);
      }
    } else if (op == "k") {
      if (PopNums(4, v)) {
        CmykToRgb(v[0], v[1], v[2], v[3], state_.fill_r, state_.fill_g, state_.fill_b);
      }
    } else if (op == "BT") {
      state_.text_matrix = PdfMatrix{};
      state_.text_line_matrix = PdfMatrix{};
    } else if (op == "ET") {
      // Nothing to flush.
    } else if (op == "Tf") {
      if (static_cast<int>(operands_.size()) >= 2) {
        const PdfObjectPtr size_obj = operands_.back();
        operands_.pop_back();
        const PdfObjectPtr name_obj = operands_.back();
        operands_.pop_back();
        state_.font_size = std::max(0.0f, ToFloat(*size_obj, 12.0f));
        state_.font = nullptr;
        if (const auto* name = GetIf<PdfName>(*name_obj)) {
          const auto it = fonts_.find(name->value);
          if (it != fonts_.end()) {
            state_.font = &it->second;
          }
        }
      }
    } else if (op == "Td") {
      if (PopNums(2, v)) {
        state_.text_line_matrix.e += v[0];
        state_.text_line_matrix.f += v[1];
        state_.text_matrix = state_.text_line_matrix;
      }
    } else if (op == "TD") {
      if (PopNums(2, v)) {
        state_.leading = -v[1];
        state_.text_line_matrix.e += v[0];
        state_.text_line_matrix.f += v[1];
        state_.text_matrix = state_.text_line_matrix;
      }
    } else if (op == "Tm") {
      if (PopNums(6, v)) {
        state_.text_matrix = PdfMatrix{v[0], v[1], v[2], v[3], v[4], v[5]};
        state_.text_line_matrix = state_.text_matrix;
      }
    } else if (op == "T*") {
      state_.text_line_matrix.f -= state_.leading;
      state_.text_matrix = state_.text_line_matrix;
    } else if (op == "TL") {
      if (PopNums(1, v)) {
        state_.leading = v[0];
      }
    } else if (op == "Tc") {
      if (PopNums(1, v)) {
        state_.char_space = v[0];
      }
    } else if (op == "Tw") {
      if (PopNums(1, v)) {
        state_.word_space = v[0];
      }
    } else if (op == "Tr") {
      if (PopNums(1, v)) {
        state_.render_mode = static_cast<int>(v[0]);
      }
    } else if (op == "Ts") {
      (void)PopNums(1, v); // text rise: accepted, not modeled (approximation)
    } else if (op == "Tz") {
      (void)PopNums(1, v); // horizontal scaling: accepted, not modeled
    } else if (op == "Tj") {
      if (!operands_.empty()) {
        const PdfObjectPtr s = operands_.back();
        operands_.pop_back();
        if (const auto* str = GetIf<PdfString>(*s)) {
          ShowText(str->value);
        }
      }
    } else if (op == "'") {
      state_.text_line_matrix.f -= state_.leading;
      state_.text_matrix = state_.text_line_matrix;
      if (!operands_.empty()) {
        const PdfObjectPtr s = operands_.back();
        operands_.pop_back();
        if (const auto* str = GetIf<PdfString>(*s)) {
          ShowText(str->value);
        }
      }
    } else if (op == "\"") {
      if (PopNums(2, v)) {
        state_.word_space = v[0];
        state_.char_space = v[1];
        if (!operands_.empty()) {
          const PdfObjectPtr s = operands_.back();
          operands_.pop_back();
          if (const auto* str = GetIf<PdfString>(*s)) {
            ShowText(str->value);
          }
        }
      }
    } else if (op == "TJ") {
      if (!operands_.empty()) {
        const PdfObjectPtr arr = operands_.back();
        operands_.pop_back();
        if (const auto* a = GetIf<PdfArray>(*arr)) {
          for (const PdfObjectPtr& item : a->items) {
            if (const auto* str = GetIf<PdfString>(*item)) {
              ShowText(str->value);
            } else {
              const float offset = ToFloat(*item, 0.0f);
              // TJ offsets are in 1/1000 em and move the pen backward.
              state_.text_matrix.e -= offset * 0.001f * state_.font_size *
                                      std::max(std::fabs(state_.text_matrix.a), 0.0001f);
            }
          }
        }
      }
    } else {
      // Operators we do not implement (Do/BI image XObjects, W/W* clipping,
      // and anything else): drop the operands they would have consumed so
      // the rest of the page continues to render.  NOT IMPLEMENTED: images,
      // clipping.
      operands_.clear();
    }
  }

  void CloseLast()
  {
    if (has_current_ && !path_.empty() && !path_.back().closed) {
      path_.back().points.push_back(path_start_);
      path_.back().closed = true;
      current_ = path_start_;
    }
  }

  static void CmykToRgb(float c, float m, float y, float k, float& r, float& g, float& b)
  {
    // Simple subtractive conversion (device CMYK approximation).
    r = (1.0f - std::min(1.0f, c)) * (1.0f - std::min(1.0f, k));
    g = (1.0f - std::min(1.0f, m)) * (1.0f - std::min(1.0f, k));
    b = (1.0f - std::min(1.0f, y)) * (1.0f - std::min(1.0f, k));
  }
};

class Parser
{
public:
  base::Result<PdfDocument> Run(std::string_view data)
  {
    if (!IsPdf(data)) {
      return base::Error::InvalidArgument("not a PDF file");
    }
    if (!LoadXref(data)) {
      return base::Error::Parse("pdf: could not locate the cross-reference table");
    }
    PdfObject root = GetTrailerRoot();
    return ExtractPages(root);
  }

  // Loads the document and renders one page (the public RenderPage wrapper).
  base::Result<image::Image> RenderPageEntry(std::string_view data, int page_index, float scale)
  {
    if (!IsPdf(data)) {
      return base::Error::InvalidArgument("not a PDF file");
    }
    if (!LoadXref(data)) {
      return base::Error::Parse("pdf: could not locate the cross-reference table");
    }
    return RenderPageImage(page_index, scale);
  }

  // Renders one page to an RGBA image.  Parses the document with the same
  // xref/object machinery as Run(); see pdf.h for the supported subset.
  base::Result<image::Image> RenderPageImage(int page_index, float scale)
  {
    if (page_index < 0 || scale <= 0.0f || !std::isfinite(scale)) {
      return base::Error::InvalidArgument("pdf: bad page index or scale");
    }
    PdfObject root = GetTrailerRoot();
    int remaining = page_index;
    const PdfDict* page_dict = nullptr;
    PdfObject root_resolved = Resolve(root);
    if (const auto* rd = GetIf<std::shared_ptr<PdfDict>>(root_resolved)) {
      page_dict = FindPageDict(**rd, remaining);
    }
    if (page_dict == nullptr) {
      return base::Error::Parse("pdf: page index out of range");
    }

    // MediaBox in points; the PDF default is US Letter (612x792).  The
    // attribute may be inherited from ancestor nodes of the page tree
    // (PDF 1.7 §7.7.3.4).
    float page_w = 612, page_h = 792;
    if (PdfObject mb = FindInheritedAttribute(page_dict, "MediaBox");
        !std::holds_alternative<std::nullptr_t>(mb.value)) {
      if (const auto* arr = GetIf<PdfArray>(mb); arr != nullptr && arr->items.size() >= 4) {
        const auto num_at = [&](std::size_t i) -> float {
          if (const auto* v = GetIf<int64_t>(*arr->items[i])) {
            return static_cast<float>(*v);
          }
          if (const auto* v = GetIf<double>(*arr->items[i])) {
            return static_cast<float>(*v);
          }
          return 0.0f;
        };
        page_w = std::fabs(num_at(2) - num_at(0));
        page_h = std::fabs(num_at(3) - num_at(1));
      }
    }
    if (page_w <= 0 || page_h <= 0 || page_w > 100000 || page_h > 100000) {
      return base::Error::Parse("pdf: bad MediaBox");
    }
    const int px_w = std::max(1, static_cast<int>(std::lround(page_w * scale)));
    const int px_h = std::max(1, static_cast<int>(std::lround(page_h * scale)));
    // 32 M pixels: the same canvas budget as the GIF/AVIF decoders.
    if (static_cast<std::size_t>(px_w) * static_cast<std::size_t>(px_h) >
        (128u * 1024u * 1024u) / 4u) {
      return base::Error::Parse("pdf: page too large to render");
    }

    const std::map<std::string, PdfFontMetrics> fonts = CollectFontMetrics(*page_dict);
    PdfPageRenderer renderer(fonts, px_w, px_h, scale, &fonts_registry_);
    return renderer.Run(CollectPageContent(*page_dict));
  }

private:
  struct XrefEntry
  {
    int type = 0;           // 1 = classic offset, 2 = inside an object stream
    size_t offset = 0;      // type 1
    int64_t objstm_num = 0; // type 2: the /ObjStm container object number
    int objstm_index = 0;   // type 2: the object's index inside the container
  };
  std::map<int64_t, size_t> offsets_;
  std::map<int64_t, XrefEntry> xref_entries_;
  std::map<int64_t, PdfObjectPtr> objects_;
  // Objects inside a compressed object stream, keyed by object number.
  std::map<int64_t, PdfObjectPtr> stream_objects_;
  std::string trailer_data_; // kept for trailer parsing
  std::string_view file_;
  // The xref-stream dictionary, when the document uses one (its /Root etc.
  // act as the trailer).
  std::shared_ptr<PdfDict> xref_stream_dict_;
  // FreeType glyph source for the text renderer.
  graphics::FontRegistry fonts_registry_;

  bool LoadXref(std::string_view data)
  {
    file_ = data;
    size_t startxref = std::string_view::npos;
    for (size_t i = 0; (i = data.find("startxref", i)) != std::string_view::npos; i += 9) {
      startxref = i;
    }
    if (startxref == std::string_view::npos)
      return false;
    size_t pos = startxref + 9;
    SkipWsAndComments(data, pos);
    const size_t line_end = data.find_first_of(" \t\r\n", pos);
    int64_t xref_offset = 0;
    if (!ParseInt(data.substr(pos, line_end - pos), &xref_offset))
      return false;
    if (ReadXrefSection(static_cast<size_t>(xref_offset))) {
      return true;
    }
    // PDF 1.5+: the startxref offset points at a cross-reference stream
    // object instead of a classic table.
    return LoadXrefStream(static_cast<size_t>(xref_offset));
  }

  // Reads the raw bytes of the stream object at |offset| (dict + body)
  // without using EnsureObjectLoaded, for bootstrapping xref streams.
  bool ReadRawStreamAt(size_t offset, std::shared_ptr<PdfDict>& dict, std::string& raw)
  {
    size_t pos = offset;
    const auto n = ReadNumberToken(file_, pos);
    if (!n.has_value())
      return false;
    const auto g = ReadNumberToken(file_, pos);
    if (!g.has_value())
      return false;
    SkipWsAndComments(file_, pos);
    if (file_.substr(pos, 3) != "obj")
      return false;
    pos += 3;
    PdfObjectPtr obj = ParseObjectPtr(file_, pos, 0);
    const auto* dict_ptr = GetIf<std::shared_ptr<PdfDict>>(*obj);
    if (dict_ptr == nullptr)
      return false;
    SkipWsAndComments(file_, pos);
    if (file_.substr(pos, 6) != "stream")
      return false;
    pos += 6;
    if (pos < file_.size() && file_[pos] == '\r')
      ++pos;
    if (pos < file_.size() && file_[pos] == '\n')
      ++pos;
    const size_t stream_start = pos;
    const size_t end = file_.find("endstream", pos);
    if (end == std::string_view::npos)
      return false;
    size_t len = end - stream_start;
    if (len >= 2 && file_[stream_start + len - 2] == '\r' &&
        file_[stream_start + len - 1] == '\n') {
      len -= 2;
    } else if (len >= 1 && file_[stream_start + len - 1] == '\n') {
      len -= 1;
    }
    dict = *dict_ptr;
    raw.assign(file_.substr(stream_start, len));
    return true;
  }

  // Parses a cross-reference stream (PDF 1.7 §7.5.8) and, through it, the
  // /Prev chain.  Returns true when at least one section was read.
  bool LoadXrefStream(size_t offset)
  {
    for (int guard = 0; guard < 64; ++guard) {
      std::shared_ptr<PdfDict> dict;
      std::string raw;
      if (offset >= file_.size() || !ReadRawStreamAt(offset, dict, raw)) {
        return false;
      }
      if (xref_stream_dict_ == nullptr) {
        xref_stream_dict_ = dict;
      }
      // /W = per-entry byte widths [w0 w1 w2].
      std::array<int, 3> w{1, 0, 0};
      const PdfObject* widths = dict->Find("W");
      if (widths != nullptr) {
        if (const auto* arr = GetIf<PdfArray>(*widths)) {
          for (std::size_t i = 0; i < arr->items.size() && i < 3; ++i) {
            if (const auto* v = GetIf<int64_t>(*arr->items[i])) {
              w[i] = static_cast<int>(*v);
            }
          }
        }
      }
      int64_t size = 0;
      if (const PdfObject* sz = dict->Find("Size"); sz != nullptr) {
        if (const auto* v = GetIf<int64_t>(*sz))
          size = *v;
      }
      // /Index [first count ...]: default [0 Size].
      std::vector<std::pair<int64_t, int64_t>> sections;
      if (const PdfObject* idx = dict->Find("Index"); idx != nullptr) {
        if (const auto* arr = GetIf<PdfArray>(*idx)) {
          for (std::size_t i = 0; i + 1 < arr->items.size(); i += 2) {
            int64_t first = 0, count = 0;
            if (const auto* v = GetIf<int64_t>(*arr->items[i]))
              first = *v;
            if (const auto* v = GetIf<int64_t>(*arr->items[i + 1]))
              count = *v;
            sections.push_back({first, count});
          }
        }
      } else if (size > 0) {
        sections.push_back({0, size});
      }
      std::string inflated = raw;
      if (const PdfObject* filter = dict->Find("Filter"); filter != nullptr) {
        if (const auto* name = GetIf<PdfName>(*filter);
            name != nullptr && name->value == "FlateDecode") {
          const auto r = InflateZlib(raw);
          if (!r)
            return false;
          inflated = std::move(r.value());
        }
      }
      const size_t entry_bytes =
          static_cast<size_t>(w[0]) + static_cast<size_t>(w[1]) + static_cast<size_t>(w[2]);
      if (entry_bytes == 0)
        return false;
      size_t pos = 0;
      for (const auto& [first, count] : sections) {
        if (count <= 0 || count > 10000000)
          continue;
        for (int64_t i = 0; i < count; ++i) {
          if (pos + entry_bytes > inflated.size())
            return false;
          auto read_int = [&](int bytes) -> int64_t {
            int64_t v = 0;
            for (int b = 0; b < bytes; ++b) {
              v = (v << 8) | static_cast<unsigned char>(inflated[pos++]);
            }
            return v;
          };
          const int type = w[0] > 0 ? static_cast<int>(read_int(w[0])) : 1;
          XrefEntry entry;
          entry.type = type;
          if (type == 1) {
            entry.offset = static_cast<size_t>(read_int(w[1]));
            if (w[2] > 0)
              read_int(w[2]); // generation number (unused)
          } else if (type == 2) {
            entry.objstm_num = read_int(w[1]);
            entry.objstm_index = w[2] > 0 ? static_cast<int>(read_int(w[2])) : 0;
          } else {
            if (w[1] > 0)
              read_int(w[1]);
            if (w[2] > 0)
              read_int(w[2]);
            continue; // free entry
          }
          xref_entries_.emplace(first + i, entry);
        }
      }
      // /Prev chains to the older section.
      int64_t prev = 0;
      if (const PdfObject* p = dict->Find("Prev"); p != nullptr) {
        if (const auto* v = GetIf<int64_t>(*p))
          prev = *v;
      }
      if (prev > 0) {
        offset = static_cast<size_t>(prev);
        continue;
      }
      return true;
    }
    return false;
  }

  bool ReadXrefSection(size_t offset)
  {
    for (int guard = 0; guard < 64; ++guard) {
      if (offset >= file_.size())
        return false;
      size_t pos = offset;
      SkipWsAndComments(file_, pos);
      if (file_.substr(pos, 4) == "xref") {
        pos += 4;
        for (;;) {
          SkipWsAndComments(file_, pos);
          if (file_.substr(pos, 7) == "trailer") {
            pos += 7;
            SkipWsAndComments(file_, pos);
            if (trailer_data_.empty())
              trailer_data_ = std::string(file_);
            PdfObject trailer = *ParseObjectPtr(file_, pos, 0);
            const auto* td = GetIf<std::shared_ptr<PdfDict>>(trailer);
            if (td != nullptr) {
              if (const PdfObject* p = (*td)->Find("Prev"); p != nullptr) {
                if (const auto* v = GetIf<int64_t>(*p); v != nullptr && *v > 0) {
                  offset = static_cast<size_t>(*v);
                  break; // continue the outer guard loop with the older table
                }
              }
            }
            return true;
          }
          // Subsection header "first count".
          int64_t first = 0, count = 0;
          const auto n1 = ReadNumberToken(file_, pos);
          if (!n1.has_value())
            return true;
          first = n1.value();
          const auto n2 = ReadNumberToken(file_, pos);
          if (!n2.has_value())
            return true;
          count = n2.value();
          if (count <= 0 || count > 1000000)
            return false;
          SkipWsAndComments(file_, pos); // newline between header and entries
          for (int64_t i = 0; i < count; ++i) {
            if (pos + 20 > file_.size())
              return false;
            const std::string_view entry = file_.substr(pos, 20);
            pos += 20;
            const char type = entry.size() >= 18 ? entry[17] : ' ';
            if (type == 'n') {
              int64_t obj_offset = 0;
              if (ParseInt(entry.substr(0, 10), &obj_offset)) {
                // The first (latest) xref section wins; /Prev sections only
                // fill in objects that are not already known.
                offsets_.emplace(first + i, static_cast<size_t>(obj_offset));
              }
            }
          }
        }
      } else if (file_.substr(pos, 7) == "trailer") {
        if (trailer_data_.empty())
          trailer_data_ = std::string(file_);
        return true;
      } else {
        return false;
      }
    }
    return false;
  }

  PdfObject GetTrailerRoot()
  {
    // PDF 1.5+: the xref stream dictionary doubles as the trailer.
    if (xref_stream_dict_ != nullptr) {
      if (const PdfObject* root = xref_stream_dict_->Find("Root"); root != nullptr) {
        return *root;
      }
    }
    if (trailer_data_.empty())
      return PdfObject(nullptr);
    size_t pos = trailer_data_.find("trailer");
    if (pos == std::string_view::npos)
      return PdfObject(nullptr);
    pos += 7;
    SkipWsAndComments(trailer_data_, pos);
    PdfObject trailer = *ParseObjectPtr(trailer_data_, pos, 0);
    const auto* td = GetIf<std::shared_ptr<PdfDict>>(trailer);
    if (td == nullptr)
      return PdfObject(nullptr);
    const PdfObject* root = (*td)->Find("Root");
    if (root == nullptr)
      return PdfObject(nullptr);
    return *root;
  }

  // Parses an /ObjStm object stream (PDF 1.7 §7.5.7) and caches every
  // object it contains, keyed by object number.  Pair offsets are relative
  // to the first object in the stream, whose position is /First.
  void LoadObjectStream(const PdfDict& dict)
  {
    const PdfObject* stream_obj = dict.Find("__stream__");
    if (stream_obj == nullptr)
      return;
    const auto* stream = GetIf<PdfString>(*stream_obj);
    if (stream == nullptr)
      return;
    int64_t n = 0;
    if (const PdfObject* nobj = dict.Find("N"); nobj != nullptr) {
      if (const auto* v = GetIf<int64_t>(*nobj))
        n = *v;
    }
    int64_t first = 0;
    if (const PdfObject* fobj = dict.Find("First"); fobj != nullptr) {
      if (const auto* v = GetIf<int64_t>(*fobj))
        first = *v;
    }
    const std::string& data = stream->value;
    size_t pos = 0;
    SkipWsAndComments(data, pos);
    std::vector<std::pair<int64_t, int64_t>> pairs;
    for (int64_t i = 0; i < n; ++i) {
      const auto num = ReadNumberToken(data, pos);
      if (!num.has_value())
        return;
      const auto off = ReadNumberToken(data, pos);
      if (!off.has_value())
        return;
      pairs.push_back({*num, *off});
      if (pairs.size() > 100000)
        return;
    }
    for (const auto& [obj_num, obj_off] : pairs) {
      const int64_t abs = first + obj_off;
      if (obj_num < 0 || abs < 0 || static_cast<size_t>(abs) >= data.size()) {
        continue;
      }
      size_t p = static_cast<size_t>(abs);
      PdfObjectPtr obj = ParseObjectPtr(data, p, 0);
      stream_objects_.emplace(obj_num, std::move(obj));
    }
  }

  void EnsureObjectLoaded(int64_t num)
  {
    if (objects_.count(num) != 0 || stream_objects_.count(num) != 0)
      return;
    const auto xit = xref_entries_.find(num);
    if (xit != xref_entries_.end()) {
      if (xit->second.type == 2) {
        // Object lives inside a compressed object stream.
        const int64_t container = xit->second.objstm_num;
        EnsureObjectLoaded(container);
        const auto cit = objects_.find(container);
        if (cit == objects_.end())
          return;
        const auto* dict_ptr = GetIf<std::shared_ptr<PdfDict>>(*cit->second);
        if (dict_ptr == nullptr)
          return;
        LoadObjectStream(**dict_ptr);
        return;
      }
      // Fall through to the classic offset path below.
    }
    const auto it = offsets_.find(num);
    if (it == offsets_.end()) {
      if (xit == xref_entries_.end())
        return;
    }
    size_t offset = 0;
    if (it != offsets_.end()) {
      offset = it->second;
    } else if (xit != xref_entries_.end()) {
      offset = xit->second.offset;
    }
    if (offset >= file_.size())
      return;
    size_t pos = offset;
    const auto n = ReadNumberToken(file_, pos);
    if (!n.has_value() || n.value() != num)
      return;
    const auto g = ReadNumberToken(file_, pos);
    if (!g.has_value())
      return;
    SkipWsAndComments(file_, pos);
    if (file_.substr(pos, 3) != "obj")
      return;
    pos += 3;
    PdfObjectPtr obj = ParseObjectPtr(file_, pos, 0);

    // Streams: after the dict, expect "stream" + data + "endstream".
    if (const auto* dict_ptr = GetIf<std::shared_ptr<PdfDict>>(*obj)) {
      SkipWsAndComments(file_, pos);
      if (file_.substr(pos, 6) == "stream") {
        pos += 6;
        if (pos < file_.size() && file_[pos] == '\r')
          ++pos;
        if (pos < file_.size() && file_[pos] == '\n')
          ++pos;
        const size_t stream_start = pos;
        const size_t end = file_.find("endstream", pos);
        if (end != std::string_view::npos) {
          size_t len = end - stream_start;
          if (len >= 2 && file_[stream_start + len - 2] == '\r' &&
              file_[stream_start + len - 1] == '\n') {
            len -= 2;
          } else if (len >= 1 && file_[stream_start + len - 1] == '\n') {
            len -= 1;
          }
          const std::string raw(file_.substr(stream_start, len));
          PdfString stream_bytes{DecodeStream(**dict_ptr, raw)};
          auto with_stream = std::make_shared<PdfDict>(**dict_ptr);
          with_stream->entries["__stream__"] = std::make_shared<PdfObject>(std::move(stream_bytes));
          obj = std::make_shared<PdfObject>(with_stream);
        }
      }
    }
    objects_[num] = std::move(obj);
  }

  // Decodes a stream body according to /Filter and /DecodeParms.
  std::string DecodeStream(const PdfDict& dict, const std::string& raw)
  {
    std::vector<std::string> filters;
    if (const PdfObject* filter = dict.Find("Filter"); filter != nullptr) {
      if (const auto* name = GetIf<PdfName>(*filter)) {
        filters.push_back(name->value);
      } else if (const auto* arr = GetIf<PdfArray>(*filter)) {
        for (const PdfObjectPtr& f : arr->items) {
          if (const auto* fname = GetIf<PdfName>(*f))
            filters.push_back(fname->value);
        }
      }
    }
    if (filters.empty())
      return raw;
    if (filters.size() != 1 || filters[0] != "FlateDecode") {
      return {}; // unsupported filter chain: stream contributes no text
    }
    auto inflated = InflateZlib(raw);
    if (!inflated)
      return {};
    std::string out = std::move(inflated.value());

    int predictor = 1, colors = 1, bpc = 8, columns = 1;
    if (const PdfObject* parms = dict.Find("DecodeParms"); parms != nullptr) {
      if (const auto* pd = GetIf<std::shared_ptr<PdfDict>>(*parms)) {
        auto getint = [&](const char* key, int def) {
          const PdfObject* v = (*pd)->Find(key);
          if (v == nullptr)
            return def;
          if (const auto* i = GetIf<int64_t>(*v))
            return static_cast<int>(*i);
          return def;
        };
        predictor = getint("Predictor", 1);
        colors = getint("Colors", 1);
        bpc = getint("BitsPerComponent", 8);
        columns = getint("Columns", 1);
      }
    }
    if (predictor != 1 && !ApplyPredictor(predictor, colors, bpc, columns, out)) {
      return {};
    }
    return out;
  }

  PdfObject Resolve(PdfObject obj, int depth = 0)
  {
    if (depth > 64)
      return PdfObject(nullptr);
    if (const auto* ref = GetIf<PdfRef>(obj)) {
      EnsureObjectLoaded(ref->num);
      const auto it = objects_.find(ref->num);
      if (it != objects_.end()) {
        return Resolve(*it->second, depth + 1);
      }
      const auto sit = stream_objects_.find(ref->num);
      if (sit != stream_objects_.end()) {
        return Resolve(*sit->second, depth + 1);
      }
      return PdfObject(nullptr);
    }
    return obj;
  }

  // Resolves an inheritable page attribute (PDF 1.7 §7.7.3.4) by walking
  // the /Parent chain.  Returns null when neither the page nor any
  // ancestor defines it.
  PdfObject FindInheritedAttribute(const PdfDict* node, std::string_view key)
  {
    for (int guard = 0; guard < 128 && node != nullptr; ++guard) {
      if (const PdfObject* v = node->Find(key); v != nullptr) {
        return Resolve(*v);
      }
      const PdfObject* parent = node->Find("Parent");
      if (parent == nullptr)
        break;
      PdfObject parent_obj = Resolve(*parent);
      const auto* parent_dict = GetIf<std::shared_ptr<PdfDict>>(parent_obj);
      node = parent_dict == nullptr ? nullptr : parent_dict->get();
    }
    return PdfObject(nullptr);
  }

  base::Result<PdfDocument> ExtractPages(PdfObject root)
  {
    PdfDocument doc;
    PdfObject catalog_obj = Resolve(root); // /Root is usually an indirect ref
    const auto* catalog = GetIf<std::shared_ptr<PdfDict>>(catalog_obj);
    if (catalog == nullptr) {
      return base::Error::Parse("pdf: missing catalog");
    }
    // Catalog -> /Pages tree root.
    const PdfObject* pages = (*catalog)->Find("Pages");
    if (pages == nullptr) {
      return base::Error::Parse("pdf: missing pages tree");
    }
    PdfObject pages_obj = Resolve(*pages);
    if (const auto* pages_dict = GetIf<std::shared_ptr<PdfDict>>(pages_obj)) {
      CollectPages(**pages_dict, doc);
    }
    if (doc.pages.empty()) {
      return base::Error::Parse("pdf: no pages found");
    }

    // Title from /Info /Title.
    if (!trailer_data_.empty()) {
      size_t pos = trailer_data_.find("trailer");
      if (pos != std::string_view::npos) {
        pos += 7;
        SkipWsAndComments(trailer_data_, pos);
        PdfObject trailer = *ParseObjectPtr(trailer_data_, pos, 0);
        if (const auto* td = GetIf<std::shared_ptr<PdfDict>>(trailer)) {
          if (const PdfObject* info = (*td)->Find("Info"); info != nullptr) {
            PdfObject info_obj = Resolve(*info);
            if (const auto* id = GetIf<std::shared_ptr<PdfDict>>(info_obj)) {
              if (const PdfObject* title = (*id)->Find("Title"); title != nullptr) {
                if (const auto* ts = GetIf<PdfString>(*title)) {
                  doc.title = DecodeTextBytes(ts->value);
                }
              }
            }
          }
        }
      }
    }
    return doc;
  }

  // Walks the pages tree to the page at 0-based |remaining| (decrementing),
  // returning its dictionary or nullptr when the index is out of range.  The
  // catalog points at the tree root via /Pages; tree nodes list children
  // via /Kids.
  const PdfDict* FindPageDict(const PdfDict& node, int& remaining)
  {
    if (const PdfObject* kids = node.Find("Kids"); kids != nullptr) {
      PdfObject kids_resolved = Resolve(*kids);
      if (const auto* arr = GetIf<PdfArray>(kids_resolved)) {
        for (const PdfObjectPtr& kid : arr->items) {
          PdfObject resolved = Resolve(*kid);
          if (const auto* d = GetIf<std::shared_ptr<PdfDict>>(resolved)) {
            if (const PdfDict* found = FindPageDict(**d, remaining)) {
              return found;
            }
          }
        }
      }
      return nullptr;
    }
    if (const PdfObject* pages = node.Find("Pages"); pages != nullptr) {
      PdfObject pages_resolved = Resolve(*pages);
      if (const auto* d = GetIf<std::shared_ptr<PdfDict>>(pages_resolved)) {
        return FindPageDict(**d, remaining);
      }
      return nullptr;
    }
    if (remaining == 0) {
      return &node;
    }
    --remaining;
    return nullptr;
  }

  // Extracts per-font metrics (/Widths, /FirstChar) from the page's
  // /Resources /Font dictionary for the text renderer.
  std::map<std::string, PdfFontMetrics> CollectFontMetrics(const PdfDict& page)
  {
    std::map<std::string, PdfFontMetrics> out;
    const PdfObject* resources = page.Find("Resources");
    if (resources == nullptr) {
      return out;
    }
    PdfObject res = Resolve(*resources);
    const auto* rd = GetIf<std::shared_ptr<PdfDict>>(res);
    if (rd == nullptr) {
      return out;
    }
    const PdfObject* fonts = (*rd)->Find("Font");
    if (fonts == nullptr) {
      return out;
    }
    PdfObject f = Resolve(*fonts);
    const auto* fd = GetIf<std::shared_ptr<PdfDict>>(f);
    if (fd == nullptr) {
      return out;
    }
    for (const auto& [name, obj] : (*fd)->entries) {
      PdfObject font_obj = Resolve(*obj);
      const auto* font_dict = GetIf<std::shared_ptr<PdfDict>>(font_obj);
      if (font_dict == nullptr) {
        continue;
      }
      PdfFontMetrics metrics;
      if (const PdfObject* fc = (*font_dict)->Find("FirstChar"); fc != nullptr) {
        if (const auto* i = GetIf<int64_t>(*fc)) {
          metrics.first_char = static_cast<int>(*i);
        }
      }
      if (const PdfObject* widths = (*font_dict)->Find("Widths"); widths != nullptr) {
        PdfObject wa = Resolve(*widths);
        if (const auto* arr = GetIf<PdfArray>(wa)) {
          for (const PdfObjectPtr& item : arr->items) {
            float w = 0;
            if (const auto* i = GetIf<int64_t>(*item)) {
              w = static_cast<float>(*i);
            } else if (const auto* d = GetIf<double>(*item)) {
              w = static_cast<float>(*d);
            }
            metrics.widths.push_back(w);
          }
        }
      }
      out[name] = std::move(metrics);
    }
    return out;
  }

  void CollectPages(const PdfDict& node, PdfDocument& doc)
  {
    const PdfObject* kids = node.Find("Kids");
    if (kids == nullptr) {
      ExtractPage(node, doc);
      return;
    }
    if (const auto* arr = GetIf<PdfArray>(*kids)) {
      for (const PdfObjectPtr& kid : arr->items) {
        PdfObject resolved = Resolve(*kid);
        if (const auto* d = GetIf<std::shared_ptr<PdfDict>>(resolved)) {
          CollectPages(**d, doc);
        }
      }
    }
  }

  // Concatenates a page's content streams (/Contents: one stream or an array
  // of streams), each already decoded by EnsureObjectLoaded.
  std::string CollectPageContent(const PdfDict& page)
  {
    std::string content;
    const PdfObject* contents = page.Find("Contents");
    if (contents == nullptr) {
      return content;
    }
    PdfObject resolved = Resolve(*contents);
    const auto append_stream = [&](const PdfDict& d) {
      const PdfObject* stream = d.Find("__stream__");
      if (stream == nullptr) {
        return;
      }
      if (const auto* s = GetIf<PdfString>(*stream)) {
        content += s->value;
        content += '\n';
      }
    };
    if (const auto* dict = GetIf<std::shared_ptr<PdfDict>>(resolved)) {
      append_stream(**dict);
    } else if (const auto* arr = GetIf<PdfArray>(resolved)) {
      for (const PdfObjectPtr& item : arr->items) {
        PdfObject stream_obj = Resolve(*item);
        if (const auto* d = GetIf<std::shared_ptr<PdfDict>>(stream_obj)) {
          append_stream(**d);
        }
      }
    }
    return content;
  }

  void ExtractPage(const PdfDict& page, PdfDocument& doc)
  {
    PdfPage out;
    out.index = static_cast<int>(doc.pages.size());

    // MediaBox may be inherited from the page-tree ancestors.
    if (PdfObject mb = FindInheritedAttribute(&page, "MediaBox");
        !std::holds_alternative<std::nullptr_t>(mb.value)) {
      if (const auto* arr = GetIf<PdfArray>(mb); arr != nullptr && arr->items.size() >= 4) {
        auto num_at = [&](size_t i) -> int {
          if (const auto* v = GetIf<int64_t>(*arr->items[i])) {
            return static_cast<int>(*v);
          }
          if (const auto* v = GetIf<double>(*arr->items[i])) {
            return static_cast<int>(*v);
          }
          return 0;
        };
        out.width = std::abs(num_at(2) - num_at(0));
        out.height = std::abs(num_at(3) - num_at(1));
      }
    }

    out.text = ExtractTextFromContent(CollectPageContent(page));
    doc.pages.push_back(std::move(out));
    ++doc.page_count;
  }
};

} // namespace

base::Result<PdfDocument> ExtractText(std::string_view data)
{
  Parser parser;
  return parser.Run(data);
}

base::Result<image::Image> RenderPage(std::string_view data, int page_index, float scale)
{
  Parser parser;
  return parser.RenderPageEntry(data, page_index, scale);
}

bool IsPdf(std::string_view data)
{
  return data.size() >= 5 && data.substr(0, 5) == "%PDF-";
}

} // namespace neko::pdf
