// Minimal PDF text extractor.
//
// Implements just enough of the PDF 1.7 spec to extract text from ordinary
// documents: the classic xref table (with /Prev chains), the object model,
// streams with FlateDecode (PNG/TIFF predictors), the pages tree and the
// core text operators.  See pdf.h for the explicit NOT IMPLEMENTED list.
//
// Threading: pure functions, no shared state.

#include "neko/base/status.h"
#include "neko/base/utf8.h"
#include "neko/pdf/pdf.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
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

  // Number, possibly an indirect reference "N G R".
  const auto num = ReadNumberToken(s, pos);
  if (!num.has_value())
    return std::make_shared<PdfObject>(nullptr);
  const int64_t first = num.value();
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

private:
  std::map<int64_t, size_t> offsets_;
  std::map<int64_t, PdfObjectPtr> objects_;
  std::string trailer_data_; // kept for trailer parsing
  std::string_view file_;

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
    return ReadXrefSection(static_cast<size_t>(xref_offset));
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

  void EnsureObjectLoaded(int64_t num)
  {
    if (objects_.count(num) != 0)
      return;
    const auto it = offsets_.find(num);
    if (it == offsets_.end())
      return;
    const size_t offset = it->second;
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
      if (it == objects_.end())
        return PdfObject(nullptr);
      return Resolve(*it->second, depth + 1);
    }
    return obj;
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

  void ExtractPage(const PdfDict& page, PdfDocument& doc)
  {
    PdfPage out;
    out.index = static_cast<int>(doc.pages.size());

    if (const PdfObject* mb = page.Find("MediaBox"); mb != nullptr) {
      PdfObject box = Resolve(*mb);
      if (const auto* arr = GetIf<PdfArray>(box); arr != nullptr && arr->items.size() >= 4) {
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

    std::string content;
    if (const PdfObject* contents = page.Find("Contents"); contents != nullptr) {
      PdfObject resolved = Resolve(*contents);
      if (const auto* dict = GetIf<std::shared_ptr<PdfDict>>(resolved)) {
        const PdfObject* stream = (**dict).Find("__stream__");
        if (stream != nullptr) {
          if (const auto* s = GetIf<PdfString>(*stream)) {
            content += s->value;
            content += '\n';
          }
        }
      } else if (const auto* arr = GetIf<PdfArray>(resolved)) {
        for (const PdfObjectPtr& item : arr->items) {
          PdfObject stream_obj = Resolve(*item);
          if (const auto* d = GetIf<std::shared_ptr<PdfDict>>(stream_obj)) {
            const PdfObject* stream = (**d).Find("__stream__");
            if (stream != nullptr) {
              if (const auto* s = GetIf<PdfString>(*stream)) {
                content += s->value;
                content += '\n';
              }
            }
          }
        }
      }
    }
    out.text = ExtractTextFromContent(content);
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

bool IsPdf(std::string_view data)
{
  return data.size() >= 5 && data.substr(0, 5) == "%PDF-";
}

} // namespace neko::pdf
