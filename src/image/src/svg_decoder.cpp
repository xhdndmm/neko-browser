// Minimal SVG rasterizer.
//
// Scope (see svg_decoder.h): shapes, fill/stroke, transforms, viewBox scaling.
// The XML parsing here is intentionally minimal — elements + attributes only,
// enough for the SVG constructs pages actually use in <img> logos and icons.

// GCC's optimizer sometimes mis-analyzes std::vector growth (memmove over a
// temporary single-element array) as an out-of-bounds access; the runtime is
// correct and ASan/UBSan clean.  Silence just that false positive.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overread"
#endif

#include "neko/image/svg_decoder.h"

#include "neko/base/status.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neko::image {
namespace {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Small math / geometry types
// ---------------------------------------------------------------------------

struct Point
{
  double x = 0;
  double y = 0;
};

struct Mat
{
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0; // column-major 2D affine

  static Mat Translate(double tx, double ty)
  {
    return {1, 0, 0, 1, tx, ty};
  }
  static Mat Scale(double sx, double sy)
  {
    return {sx, 0, 0, sy, 0, 0};
  }
  static Mat Rotate(double degrees)
  {
    const double rad = degrees * kPi / 180.0;
    return {std::cos(rad), std::sin(rad), -std::sin(rad), std::cos(rad), 0, 0};
  }

  Mat operator*(const Mat& o) const
  {
    return {a * o.a + c * o.b,
            b * o.a + d * o.b,
            a * o.c + c * o.d,
            b * o.c + d * o.d,
            a * o.e + c * o.f + e,
            b * o.e + d * o.f + f};
  }
  Point Apply(const Point& p) const
  {
    return {a * p.x + c * p.y + e, b * p.x + d * p.y + f};
  }
};

// ---------------------------------------------------------------------------
// Color
// ---------------------------------------------------------------------------

struct Color
{
  double r = 0, g = 0, b = 0, a = 1;
};

bool ParseHexByte(std::string_view hex, uint8_t& out)
{
  if (hex.size() != 2) {
    return false;
  }
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  const int hi = nib(hex[0]);
  const int lo = nib(hex[1]);
  if (hi < 0 || lo < 0) {
    return false;
  }
  out = static_cast<uint8_t>((hi << 4) | lo);
  return true;
}

bool ParseNamedColor(std::string_view name, Color& out)
{
  struct Named
  {
    std::string_view name;
    uint8_t r, g, b;
  };
  static constexpr Named kNamed[] = {
      {"black", 0, 0, 0},       {"white", 255, 255, 255}, {"red", 255, 0, 0},
      {"green", 0, 128, 0},     {"lime", 0, 255, 0},      {"blue", 0, 0, 255},
      {"yellow", 255, 255, 0},  {"cyan", 0, 255, 255},    {"magenta", 255, 0, 255},
      {"gray", 128, 128, 128},  {"grey", 128, 128, 128},  {"silver", 192, 192, 192},
      {"maroon", 128, 0, 0},    {"olive", 128, 128, 0},   {"purple", 128, 0, 128},
      {"teal", 0, 128, 128},    {"navy", 0, 0, 128},      {"orange", 255, 165, 0},
      {"brown", 165, 42, 42},   {"pink", 255, 192, 203},  {"gold", 255, 215, 0},
      {"transparent", 0, 0, 0},
  };
  std::string lower;
  for (const char c : name) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  for (const Named& n : kNamed) {
    if (n.name == lower) {
      out.r = n.r;
      out.g = n.g;
      out.b = n.b;
      out.a = (n.name == "transparent") ? 0.0 : 1.0;
      return true;
    }
  }
  return false;
}

// Parses a CSS/SVG color.  Returns false for "none" (treated as no paint).
bool ParseColor(std::string_view text, Color& out, bool& is_none)
{
  is_none = false;
  std::string s(text);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.erase(s.begin());
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  if (s.empty() || s == "none") {
    is_none = true;
    return false;
  }
  if (s[0] == '#') {
    const std::string_view hex(s.c_str() + 1, s.size() - 1);
    uint8_t r, g, b;
    if (hex.size() == 3) {
      const std::string rr(2, hex[0]);
      const std::string gg(2, hex[1]);
      const std::string bb(2, hex[2]);
      if (ParseHexByte(rr, r) && ParseHexByte(gg, g) && ParseHexByte(bb, b)) {
        out.r = r;
        out.g = g;
        out.b = b;
        return true;
      }
      return false;
    }
    if (hex.size() == 6 && ParseHexByte(hex.substr(0, 2), r) && ParseHexByte(hex.substr(2, 2), g) &&
        ParseHexByte(hex.substr(4, 2), b)) {
      out.r = r;
      out.g = g;
      out.b = b;
      return true;
    }
    return false;
  }
  if (s.rfind("rgb(", 0) == 0 && s.back() == ')') {
    const std::string_view inner(s.c_str() + 4, s.size() - 5);
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= inner.size(); ++i) {
      if (i == inner.size() || inner[i] == ',' || inner[i] == ' ') {
        if (i > start) {
          parts.emplace_back(inner.substr(start, i - start));
        }
        start = i + 1;
      }
    }
    if (parts.size() >= 3) {
      auto chan = [](std::string_view v) -> double {
        if (!v.empty() && v.back() == '%') {
          return std::strtod(std::string(v.substr(0, v.size() - 1)).c_str(), nullptr) * 255.0 /
                 100.0;
        }
        return std::strtod(std::string(v).c_str(), nullptr);
      };
      out.r = chan(parts[0]);
      out.g = chan(parts[1]);
      out.b = chan(parts[2]);
      return true;
    }
    return false;
  }
  return ParseNamedColor(s, out);
}

// ---------------------------------------------------------------------------
// Minimal XML/SVG element parser
// ---------------------------------------------------------------------------

std::string ToLower(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

struct Element
{
  std::string name; // lowercased
  std::vector<std::pair<std::string, std::string>> attrs;
  std::vector<Element> children;
  // Character data of this element (SVG text content).  Children named
  // "#text" carry the character data that sits between child elements, so
  // <text>a<tspan>b</tspan>c</text> keeps the "a", "b", "c" order.
  std::string text;

  const std::string* Attr(std::string_view key) const
  {
    // SVG attribute names are case-sensitive in XML (viewBox, ...); attribute
    // names are stored lowercased, so look up case-insensitively.
    const std::string lower = ToLower(key);
    for (const auto& attr : attrs) {
      if (attr.first == lower) {
        return &attr.second;
      }
    }
    return nullptr;
  }
  std::string AttrOr(std::string_view key, std::string_view dflt) const
  {
    const std::string* v = Attr(key);
    return v != nullptr ? *v : std::string(dflt);
  }
};

// Skips whitespace, an optional XML declaration and comments.
std::size_t SkipXmlProlog(std::string_view data, std::size_t pos)
{
  for (;;) {
    while (pos < data.size() && std::isspace(static_cast<unsigned char>(data[pos]))) {
      ++pos;
    }
    if (pos + 1 < data.size() && data[pos] == '<' && data[pos + 1] == '?') {
      const std::size_t end = data.find("?>", pos);
      if (end == std::string_view::npos) {
        return data.size();
      }
      pos = end + 2;
      continue;
    }
    if (pos + 3 < data.size() && data[pos] == '<' && data[pos + 1] == '!' && data[pos + 2] == '-' &&
        data[pos + 3] == '-') {
      const std::size_t end = data.find("-->", pos);
      if (end == std::string_view::npos) {
        return data.size();
      }
      pos = end + 3;
      continue;
    }
    break;
  }
  return pos;
}

bool ParseAttrValue(std::string_view data, std::size_t pos, std::string& value, std::size_t& end)
{
  if (pos >= data.size()) {
    return false;
  }
  const char quote = data[pos];
  if (quote == '"' || quote == '\'') {
    const std::size_t close = data.find(quote, pos + 1);
    if (close == std::string_view::npos) {
      return false;
    }
    value = std::string(data.substr(pos + 1, close - pos - 1));
    end = close + 1;
    return true;
  }
  std::size_t i = pos;
  while (i < data.size() && data[i] != '>' && data[i] != '/' &&
         !std::isspace(static_cast<unsigned char>(data[i]))) {
    ++i;
  }
  value = std::string(data.substr(pos, i - pos));
  end = i;
  return true;
}

// Parses one element (and recursively its children).  Returns the element and
// the position after its close tag.
bool ParseElement(std::string_view data, std::size_t pos, Element& out, std::size_t& end)
{
  std::size_t i = pos;
  if (i >= data.size() || data[i] != '<') {
    return false;
  }
  ++i;
  if (i >= data.size() || data[i] == '/' || data[i] == '!') {
    return false;
  }
  const std::size_t name_start = i;
  while (i < data.size() && data[i] != '>' && data[i] != '/' &&
         !std::isspace(static_cast<unsigned char>(data[i]))) {
    ++i;
  }
  out.name = ToLower(data.substr(name_start, i - name_start));
  if (out.name.empty()) {
    return false;
  }
  bool self_closing = false;
  for (;;) {
    while (i < data.size() && std::isspace(static_cast<unsigned char>(data[i]))) {
      ++i;
    }
    if (i >= data.size()) {
      return false;
    }
    if (data[i] == '>') {
      ++i;
      break;
    }
    if (data[i] == '/') {
      ++i;
      if (i < data.size() && data[i] == '>') {
        ++i;
        self_closing = true;
        break;
      }
      return false;
    }
    const std::size_t attr_start = i;
    while (i < data.size() && data[i] != '=' && data[i] != '>' && data[i] != '/' &&
           !std::isspace(static_cast<unsigned char>(data[i]))) {
      ++i;
    }
    const std::string attr_name = ToLower(data.substr(attr_start, i - attr_start));
    while (i < data.size() && std::isspace(static_cast<unsigned char>(data[i]))) {
      ++i;
    }
    std::string attr_value;
    if (i < data.size() && data[i] == '=') {
      ++i;
      while (i < data.size() && std::isspace(static_cast<unsigned char>(data[i]))) {
        ++i;
      }
      std::size_t value_end = i;
      if (!ParseAttrValue(data, i, attr_value, value_end)) {
        return false;
      }
      i = value_end;
    }
    if (!attr_name.empty()) {
      out.attrs.emplace_back(attr_name, attr_value);
    }
  }
  if (self_closing) {
    end = i;
    return true;
  }
  for (;;) {
    const std::size_t text_start = i;
    while (i < data.size() && data[i] != '<') {
      ++i;
    }
    if (i > text_start) {
      // Character data between child elements (kept for <text>/<tspan>).
      Element text_node;
      text_node.name = "#text";
      text_node.text = std::string(data.substr(text_start, i - text_start));
      out.children.push_back(std::move(text_node));
    }
    if (i >= data.size()) {
      return false;
    }
    if (i + 1 < data.size() && data[i + 1] == '/') {
      const std::size_t close = data.find('>', i);
      if (close == std::string_view::npos) {
        return false;
      }
      end = close + 1;
      return true;
    }
    if (i + 3 < data.size() && data[i + 1] == '!' && data[i + 2] == '-' && data[i + 3] == '-') {
      const std::size_t comment_end = data.find("-->", i);
      if (comment_end == std::string_view::npos) {
        return false;
      }
      i = comment_end + 3;
      continue;
    }
    if (i + 1 < data.size() && data[i + 1] == '?') {
      const std::size_t decl_end = data.find("?>", i);
      if (decl_end == std::string_view::npos) {
        return false;
      }
      i = decl_end + 2;
      continue;
    }
    Element child;
    std::size_t child_end = i;
    if (!ParseElement(data, i, child, child_end)) {
      const std::size_t gt = data.find('>', i);
      if (gt == std::string_view::npos) {
        return false;
      }
      i = gt + 1;
      continue;
    }
    out.children.push_back(std::move(child));
    i = child_end;
  }
}

// ---------------------------------------------------------------------------
// Path data parsing + flattening
// ---------------------------------------------------------------------------

struct PathCmd
{
  char cmd = 0;
  std::vector<double> args;
};

bool ParseNumber(const std::string& s, std::size_t& pos, double& out)
{
  while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
    ++pos;
  }
  if (pos < s.size() && s[pos] == ',') {
    ++pos;
  }
  while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
    ++pos;
  }
  const std::size_t start = pos;
  bool any = false;
  if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
    ++pos;
  }
  while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
    ++pos;
    any = true;
  }
  if (pos < s.size() && s[pos] == '.') {
    ++pos;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
      ++pos;
      any = true;
    }
  }
  if (any && pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
    const std::size_t save = pos++;
    if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
      ++pos;
    }
    if (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
      while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
        ++pos;
      }
    } else {
      pos = save;
    }
  }
  if (!any && pos == start) {
    return false;
  }
  out = std::strtod(s.substr(start, pos - start).c_str(), nullptr);
  return true;
}

// Converts a path "d" attribute into absolute commands.
std::vector<PathCmd> ParsePathData(std::string_view d)
{
  std::vector<PathCmd> commands;
  std::string s(d);
  std::size_t pos = 0;
  Point current{0, 0};
  Point start{0, 0};
  while (pos < s.size()) {
    const std::size_t pos_before = pos;
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
      ++pos;
    }
    if (pos >= s.size()) {
      break;
    }
    const char c = s[pos];
    const bool is_cmd = std::isalpha(static_cast<unsigned char>(c));
    char cmd = is_cmd ? c : 'L'; // implicit lineto after a bare coordinate pair
    if (is_cmd) {
      ++pos;
    }
    std::vector<double> args;
    if (cmd != 'z' && cmd != 'Z') {
      for (int i = 0; i < 10000; ++i) {
        double v = 0;
        if (!ParseNumber(s, pos, v)) {
          break;
        }
        args.push_back(v);
        if (pos >= s.size() || std::isalpha(static_cast<unsigned char>(s[pos]))) {
          break;
        }
      }
    }
    const bool relative = std::islower(static_cast<unsigned char>(cmd));
    const char abs_cmd = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
    auto num = [&](std::size_t k, double& v) {
      // Guard against a command with too few arguments (e.g. "d=\"M\""):
      // reading args[k] out of bounds is UB.  Missing arguments are treated
      // as 0, matching the lenient tolerance of real parsers.
      if (k >= args.size()) {
        v = 0;
        return;
      }
      v = args[k];
      if (relative &&
          (abs_cmd == 'M' || abs_cmd == 'L' || abs_cmd == 'H' || abs_cmd == 'V' || abs_cmd == 'C' ||
           abs_cmd == 'S' || abs_cmd == 'Q' || abs_cmd == 'T' || abs_cmd == 'A')) {
        if (abs_cmd == 'H') {
          v += current.x;
        } else if (abs_cmd == 'V') {
          v += current.y;
        } else if (k % 2 == 0) {
          v += current.x;
        } else {
          v += current.y;
        }
      }
    };
    switch (abs_cmd) {
    case 'M': {
      PathCmd m;
      m.cmd = 'M';
      double x, y;
      num(0, x);
      num(1, y);
      m.args = {x, y};
      current = {x, y};
      start = current;
      commands.push_back(std::move(m));
      // Subsequent coordinate pairs in an M are implicit linetos.
      for (std::size_t k = 2; k + 1 < args.size(); k += 2) {
        double nx, ny;
        num(k, nx);
        num(k + 1, ny);
        PathCmd l;
        l.cmd = 'L';
        l.args = {nx, ny};
        current = {nx, ny};
        commands.push_back(std::move(l));
      }
      break;
    }
    case 'L': {
      PathCmd l;
      l.cmd = 'L';
      for (std::size_t k = 0; k + 1 < args.size(); k += 2) {
        double x, y;
        num(k, x);
        num(k + 1, y);
        l.args.push_back(x);
        l.args.push_back(y);
        current = {x, y};
      }
      commands.push_back(std::move(l));
      break;
    }
    case 'H':
      if (!args.empty()) {
        PathCmd h;
        h.cmd = 'H';
        double x;
        num(0, x);
        h.args = {x};
        current.x = x;
        commands.push_back(std::move(h));
      }
      break;
    case 'V':
      if (!args.empty()) {
        PathCmd v;
        v.cmd = 'V';
        double y;
        num(0, y);
        v.args = {y};
        current.y = y;
        commands.push_back(std::move(v));
      }
      break;
    case 'C': {
      PathCmd curve;
      curve.cmd = 'C';
      for (std::size_t k = 0; k + 5 < args.size(); k += 6) {
        double c1x, c1y, c2x, c2y, x, y;
        num(k, c1x);
        num(k + 1, c1y);
        num(k + 2, c2x);
        num(k + 3, c2y);
        num(k + 4, x);
        num(k + 5, y);
        curve.args.insert(curve.args.end(), {c1x, c1y, c2x, c2y, x, y});
        current = {x, y};
      }
      commands.push_back(std::move(curve));
      break;
    }
    case 'S': {
      PathCmd s_cmd;
      s_cmd.cmd = 'S';
      for (std::size_t k = 0; k + 3 < args.size(); k += 4) {
        double c2x, c2y, x, y;
        num(k, c2x);
        num(k + 1, c2y);
        num(k + 2, x);
        num(k + 3, y);
        s_cmd.args.insert(s_cmd.args.end(), {c2x, c2y, x, y});
        current = {x, y};
      }
      commands.push_back(std::move(s_cmd));
      break;
    }
    case 'Q': {
      PathCmd q;
      q.cmd = 'Q';
      for (std::size_t k = 0; k + 3 < args.size(); k += 4) {
        double cx, cy, x, y;
        num(k, cx);
        num(k + 1, cy);
        num(k + 2, x);
        num(k + 3, y);
        q.args.insert(q.args.end(), {cx, cy, x, y});
        current = {x, y};
      }
      commands.push_back(std::move(q));
      break;
    }
    case 'T': {
      PathCmd t;
      t.cmd = 'T';
      for (std::size_t k = 0; k + 1 < args.size(); k += 2) {
        double x, y;
        num(k, x);
        num(k + 1, y);
        t.args.push_back(x);
        t.args.push_back(y);
        current = {x, y};
      }
      commands.push_back(std::move(t));
      break;
    }
    case 'A': {
      PathCmd a;
      a.cmd = 'A';
      for (std::size_t k = 0; k + 6 < args.size(); k += 7) {
        double rx, ry, rot, laf, sf, x, y;
        rx = args[k];
        ry = args[k + 1];
        rot = args[k + 2];
        laf = args[k + 3];
        sf = args[k + 4];
        num(k + 5, x);
        num(k + 6, y);
        a.args.insert(a.args.end(), {rx, ry, rot, laf, sf, x, y});
        current = {x, y};
      }
      commands.push_back(std::move(a));
      break;
    }
    case 'Z':
      commands.push_back(PathCmd{'Z', {}});
      current = start;
      break;
    default:
      break;
    }
    // Guarantee forward progress: an unrecognized character in "d" (e.g. '#')
    // is consumed by neither the argument loop nor the switch, which would
    // otherwise loop forever.
    if (pos_before == pos) {
      ++pos;
    }
  }
  return commands;
}

void FlattenCubic(std::vector<Point>& out, Point p0, Point p1, Point p2, Point p3, int depth)
{
  const double d1 = std::abs((p1.x - p0.x) * (p3.y - p0.y) - (p1.y - p0.y) * (p3.x - p0.x));
  const double d2 = std::abs((p2.x - p0.x) * (p3.y - p0.y) - (p2.y - p0.y) * (p3.x - p0.x));
  if (depth <= 0 || (d1 < 0.25 && d2 < 0.25)) {
    out.push_back(p3);
    return;
  }
  const Point p01{(p0.x + p1.x) / 2, (p0.y + p1.y) / 2};
  const Point p12{(p1.x + p2.x) / 2, (p1.y + p2.y) / 2};
  const Point p23{(p2.x + p3.x) / 2, (p2.y + p3.y) / 2};
  const Point p012{(p01.x + p12.x) / 2, (p01.y + p12.y) / 2};
  const Point p123{(p12.x + p23.x) / 2, (p12.y + p23.y) / 2};
  const Point mid{(p012.x + p123.x) / 2, (p012.y + p123.y) / 2};
  FlattenCubic(out, p0, p01, p012, mid, depth - 1);
  FlattenCubic(out, mid, p123, p23, p3, depth - 1);
}

void FlattenQuadratic(std::vector<Point>& out, Point p0, Point p1, Point p2, int depth)
{
  const double d = std::abs((p1.x - p0.x) * (p2.y - p0.y) - (p1.y - p0.y) * (p2.x - p0.x));
  if (depth <= 0 || d < 0.25) {
    out.push_back(p2);
    return;
  }
  const Point p01{(p0.x + p1.x) / 2, (p0.y + p1.y) / 2};
  const Point p12{(p1.x + p2.x) / 2, (p1.y + p2.y) / 2};
  const Point mid{(p01.x + p12.x) / 2, (p01.y + p12.y) / 2};
  FlattenQuadratic(out, p0, p01, mid, depth - 1);
  FlattenQuadratic(out, mid, p12, p2, depth - 1);
}

void FlattenArc(std::vector<Point>& out,
                Point p0,
                double rx,
                double ry,
                double rot_deg,
                bool large_arc,
                bool sweep,
                Point p1)
{
  if (rx == 0 || ry == 0 || (p0.x == p1.x && p0.y == p1.y)) {
    out.push_back(p1);
    return;
  }
  const double phi = rot_deg * kPi / 180.0;
  const double cp = std::cos(phi);
  const double sp = std::sin(phi);
  const double x1pp = cp * (p0.x - p1.x) / 2 + sp * (p0.y - p1.y) / 2;
  const double y1pp = -sp * (p0.x - p1.x) / 2 + cp * (p0.y - p1.y) / 2;
  double rx2 = rx * rx;
  double ry2 = ry * ry;
  const double x1pp2 = x1pp * x1pp;
  const double y1pp2 = y1pp * y1pp;
  double lambda = x1pp2 / rx2 + y1pp2 / ry2;
  if (lambda > 1) {
    const double s = std::sqrt(lambda);
    rx *= s;
    ry *= s;
    rx2 = rx * rx;
    ry2 = ry * ry;
  }
  const double den = rx2 * y1pp2 + ry2 * x1pp2;
  const double radicand =
      den != 0 ? std::max(0.0, (rx2 * ry2 - rx2 * y1pp2 - ry2 * x1pp2) / den) : 0.0;
  const double coef = (large_arc == sweep ? -1.0 : 1.0) * std::sqrt(radicand);
  const double cxp = coef * ((rx * y1pp) / ry);
  const double cyp = coef * (-(ry * x1pp) / rx);
  const double cx = cp * cxp - sp * cyp + (p0.x + p1.x) / 2;
  const double cy = sp * cxp + cp * cyp + (p0.y + p1.y) / 2;
  auto angle = [](double ux, double uy, double vx, double vy) {
    const double dot = ux * vx + uy * vy;
    const double len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
    const double cosv = std::max(-1.0, std::min(1.0, dot / (len != 0 ? len : 1)));
    double a = std::acos(cosv);
    if (ux * vy - uy * vx < 0) {
      a = -a;
    }
    return a;
  };
  const double ux = (x1pp - cxp) / rx;
  const double uy = (y1pp - cyp) / ry;
  const double vx = (-x1pp - cxp) / rx;
  const double vy = (-y1pp - cyp) / ry;
  double theta1 = angle(1.0, 0.0, ux, uy);
  double dtheta = angle(ux, uy, vx, vy);
  if (!sweep && dtheta > 0) {
    dtheta -= 2.0 * kPi;
  }
  if (sweep && dtheta < 0) {
    dtheta += 2.0 * kPi;
  }
  const int steps = std::max(2, static_cast<int>(std::ceil(std::abs(dtheta) / (kPi / 24.0))));
  for (int i = 1; i <= steps; ++i) {
    const double t = theta1 + dtheta * static_cast<double>(i) / static_cast<double>(steps);
    const double ct = std::cos(t);
    const double st = std::sin(t);
    out.push_back({cx + rx * ct * cp - ry * st * sp, cy + rx * ct * sp + ry * st * cp});
  }
}

// ---------------------------------------------------------------------------
// Transform parsing
// ---------------------------------------------------------------------------

bool ParseTransform(std::string_view value, Mat& out)
{
  out = Mat{};
  std::string s(value);
  std::size_t pos = 0;
  bool any = false;
  while (pos < s.size()) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
      ++pos;
    }
    if (pos >= s.size()) {
      break;
    }
    const std::size_t name_start = pos;
    while (pos < s.size() && s[pos] != '(') {
      ++pos;
    }
    const std::string name = ToLower(s.substr(name_start, pos - name_start));
    if (pos >= s.size() || s[pos] != '(') {
      break;
    }
    ++pos;
    std::vector<double> args;
    for (;;) {
      double v = 0;
      if (!ParseNumber(s, pos, v)) {
        break;
      }
      args.push_back(v);
      while (pos < s.size() &&
             (std::isspace(static_cast<unsigned char>(s[pos])) || s[pos] == ',')) {
        ++pos;
      }
      if (pos < s.size() && s[pos] == ')') {
        ++pos;
        break;
      }
    }
    if (name == "translate" && !args.empty()) {
      out = out * Mat::Translate(args[0], args.size() >= 2 ? args[1] : 0);
      any = true;
    } else if (name == "scale" && !args.empty()) {
      out = out * Mat::Scale(args[0], args.size() >= 2 ? args[1] : args[0]);
      any = true;
    } else if (name == "rotate" && !args.empty()) {
      if (args.size() >= 3) {
        out = out * Mat::Translate(args[1], args[2]) * Mat::Rotate(args[0]) *
              Mat::Translate(-args[1], -args[2]);
      } else {
        out = out * Mat::Rotate(args[0]);
      }
      any = true;
    } else if (name == "matrix" && args.size() >= 6) {
      out = out * Mat{args[0], args[1], args[2], args[3], args[4], args[5]};
      any = true;
    } else {
      break;
    }
  }
  return any;
}

// ---------------------------------------------------------------------------
// Paint: solid colours and gradients
// ---------------------------------------------------------------------------

struct GradientStop
{
  double offset = 0;
  Color color;
};

// A gradient whose coordinates are already in device space (the rasterizer
// always works on device pixels).  Linear gradients use the two-point form,
// radial gradients the SVG focal-cone form (centre + radius + focal point).
struct Gradient
{
  bool radial = false;
  double x1 = 0, y1 = 0, x2 = 0, y2 = 0;        // linear
  double cx = 0, cy = 0, r = 0, fx = 0, fy = 0; // radial
  std::vector<GradientStop> stops;
};

// What a fill/stroke resolves to: either a solid colour or a device-space
// gradient whose stops provide the colour.
struct Paint
{
  Color color;
  bool has_gradient = false;
  Gradient gradient;
};

// Gradient definitions by id (linear/radial elements anywhere in the document;
// SVG allows <defs> to appear after the shapes that use it).
using GradientDefs = std::map<std::string, const Element*>;

void CollectGradientDefs(const Element& element, GradientDefs& defs)
{
  if (element.name == "lineargradient" || element.name == "radialgradient") {
    if (const std::string* id = element.Attr("id"); id != nullptr && !id->empty()) {
      defs.emplace(ToLower(*id), &element);
    }
  }
  for (const Element& child : element.children) {
    CollectGradientDefs(child, defs);
  }
}

double Clamp01(double v)
{
  return std::max(0.0, std::min(1.0, v));
}

// A gradient attribute value: plain number or percentage.  Percentages are
// fractions of |extent| (the viewport size for userSpaceOnUse, or 1 for the
// objectBoundingBox unit square, where the caller passes 1.0).
double ParseGradientCoordinate(std::string_view text, double extent, double dflt)
{
  std::string s(text);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.erase(s.begin());
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  if (s.empty()) {
    return dflt;
  }
  const bool percent = s.back() == '%';
  if (percent) {
    s.pop_back();
  }
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || !std::isfinite(v)) {
    return dflt;
  }
  return percent ? v / 100.0 * extent : v;
}

Color LerpColor(const Color& a, const Color& b, double t)
{
  return {
      a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

// Colour of a gradient at a device pixel.  Offsets outside [0,1] extend the
// nearest stop, which is the default "pad" spread method (other spread methods
// and colour interpolation hints are NOT IMPLEMENTED).
Color GradientColorAt(const Gradient& g, double px, double py)
{
  if (g.stops.empty()) {
    return Color{};
  }
  if (g.stops.size() == 1) {
    return g.stops.front().color;
  }
  double t = 0;
  if (!g.radial) {
    const double dx = g.x2 - g.x1;
    const double dy = g.y2 - g.y1;
    const double len2 = dx * dx + dy * dy;
    t = len2 <= 1e-12 ? 0.0 : ((px - g.x1) * dx + (py - g.y1) * dy) / len2;
  } else {
    // Solve |P - (F + t*(C-F))| = t*R for the largest t >= 0.
    const double dx = g.cx - g.fx;
    const double dy = g.cy - g.fy;
    const double c2 = dx * dx + dy * dy;
    const double r2 = g.r * g.r;
    const double ux = px - g.fx;
    const double uy = py - g.fy;
    if (c2 <= 1e-12) {
      t = g.r <= 1e-12 ? 0.0 : std::sqrt(ux * ux + uy * uy) / g.r;
    } else {
      const double dot = ux * dx + uy * dy;
      const double p2 = ux * ux + uy * uy;
      const double disc = dot * dot - (c2 - r2) * p2;
      if (c2 < r2) {
        t = disc <= 0 ? 0.0 : (dot - std::sqrt(disc)) / (c2 - r2);
      } else {
        t = (dot + (disc <= 0 ? 0.0 : std::sqrt(disc))) / (c2 - r2);
      }
    }
  }
  if (!(t > 0)) {
    return g.stops.front().color;
  }
  if (t >= 1) {
    return g.stops.back().color;
  }
  for (std::size_t i = 1; i < g.stops.size(); ++i) {
    const GradientStop& hi = g.stops[i];
    if (t <= hi.offset) {
      const GradientStop& lo = g.stops[i - 1];
      const double span = hi.offset - lo.offset;
      const double local = span <= 1e-9 ? 0.0 : (t - lo.offset) / span;
      return LerpColor(lo.color, hi.color, local);
    }
  }
  return g.stops.back().color;
}

Color PaintAt(const Paint& paint, double px, double py)
{
  return paint.has_gradient ? GradientColorAt(paint.gradient, px, py) : paint.color;
}

// Average uniform scale of an affine transform (used to size radial gradients
// when the CTM is anisotropic: SVG would draw an ellipse, this renders the
// mean-radius circle and is documented as a limitation).
double AverageScale(const Mat& m)
{
  const double sx = std::sqrt(m.a * m.a + m.b * m.b);
  const double sy = std::sqrt(m.c * m.c + m.d * m.d);
  return (sx + sy) / 2.0;
}

// Resolves fill/stroke into a Paint.  |gradient_id| is the id of a url(#id)
// reference (empty for solid paints); |bbox| is the shape's object bounding box
// in user space ([x, y, w, h]) and is required by objectBoundingBox gradients.
Paint MakePaint(const Color& fallback,
                const std::string& gradient_id,
                bool none_if_unresolved,
                double alpha,
                const GradientDefs& defs,
                const double* bbox,
                double viewport_w,
                double viewport_h,
                const Mat& to_device)
{
  Paint paint;
  paint.color = fallback;
  if (gradient_id.empty()) {
    return paint;
  }
  const auto it = defs.find(ToLower(gradient_id));
  if (it == defs.end()) {
    // Unresolvable reference: no paint, unless a fallback colour was given.
    if (none_if_unresolved) {
      paint.color = Color{0, 0, 0, 0};
    }
    return paint;
  }

  // Attribute / stop inheritance through href (SVG 1.1 §13.2.3): the first
  // element in the chain that defines an attribute or has <stop> children wins.
  const Element* chain[8] = {};
  int chain_len = 0;
  const Element* current = it->second;
  while (current != nullptr && chain_len < 8) {
    chain[chain_len++] = current;
    const std::string* href = current->Attr("href");
    if (href == nullptr) {
      href = current->Attr("xlink:href");
    }
    if (href == nullptr || href->size() < 2 || (*href)[0] != '#') {
      break;
    }
    const auto parent = defs.find(ToLower(href->substr(1)));
    current = parent == defs.end() ? nullptr : parent->second;
  }
  const auto chain_attr = [&](const char* key) -> const std::string* {
    for (int i = 0; i < chain_len; ++i) {
      if (const std::string* v = chain[i]->Attr(key); v != nullptr) {
        return v;
      }
    }
    return nullptr;
  };

  std::vector<GradientStop> stops;
  for (int i = 0; i < chain_len && stops.empty(); ++i) {
    for (const Element& child : chain[i]->children) {
      if (child.name != "stop") {
        continue;
      }
      GradientStop stop;
      if (const std::string* off = child.Attr("offset")) {
        stop.offset = Clamp01(ParseGradientCoordinate(*off, 1.0, 0.0));
      }
      Color color{0, 0, 0, 1};
      if (const std::string* sc = child.Attr("stop-color")) {
        bool none = false;
        Color parsed;
        if (ParseColor(*sc, parsed, none) && !none) {
          color = parsed;
        }
      }
      if (const std::string* so = child.Attr("stop-opacity")) {
        color.a = Clamp01(std::strtod(so->c_str(), nullptr));
      }
      stop.color = color;
      // Later stops must not move backwards (SVG 1.1 §13.2.4, "if the offsets
      // are not monotonic, use the previous offset").
      if (!stops.empty() && stop.offset < stops.back().offset) {
        stop.offset = stops.back().offset;
      }
      stops.push_back(stop);
    }
  }
  if (stops.size() < 2) {
    if (stops.size() == 1) {
      paint.color = stops.front().color;
    }
    return paint;
  }
  for (GradientStop& stop : stops) {
    stop.color.a *= alpha;
  }

  const std::string* units = chain_attr("gradientunits");
  const bool object_bounding_box = units == nullptr || ToLower(*units) != "userspaceonuse";
  const double unit_w = object_bounding_box ? 1.0 : viewport_w;
  const double unit_h = object_bounding_box ? 1.0 : viewport_h;
  Mat gradient_transform;
  if (const std::string* gt = chain_attr("gradienttransform")) {
    if (!ParseTransform(*gt, gradient_transform)) {
      gradient_transform = Mat{};
    }
  }
  const double bw = bbox != nullptr ? bbox[2] : 1.0;
  const double bh = bbox != nullptr ? bbox[3] : 1.0;
  const double bx = bbox != nullptr ? bbox[0] : 0.0;
  const double by = bbox != nullptr ? bbox[1] : 0.0;
  // Gradient space -> user space -> device space.
  const auto to_device_point = [&](double gx, double gy) -> Point {
    Point p = gradient_transform.Apply({gx, gy});
    if (object_bounding_box) {
      p = {bx + p.x * bw, by + p.y * bh};
    }
    return to_device.Apply(p);
  };

  Gradient gradient;
  gradient.radial = it->second->name == "radialgradient";
  if (!gradient.radial) {
    const double x1 = ParseGradientCoordinate(
        chain_attr("x1") != nullptr ? *chain_attr("x1") : std::string(), unit_w, 0.0);
    const double y1 = ParseGradientCoordinate(
        chain_attr("y1") != nullptr ? *chain_attr("y1") : std::string(), unit_h, 0.0);
    const double x2 = ParseGradientCoordinate(
        chain_attr("x2") != nullptr ? *chain_attr("x2") : std::string(), unit_w, 1.0);
    const double y2 = ParseGradientCoordinate(
        chain_attr("y2") != nullptr ? *chain_attr("y2") : std::string(), unit_h, 0.0);
    const Point p1 = to_device_point(x1, y1);
    const Point p2 = to_device_point(x2, y2);
    gradient.x1 = p1.x;
    gradient.y1 = p1.y;
    gradient.x2 = p2.x;
    gradient.y2 = p2.y;
  } else {
    const double cx = ParseGradientCoordinate(
        chain_attr("cx") != nullptr ? *chain_attr("cx") : std::string(), unit_w, 0.5);
    const double cy = ParseGradientCoordinate(
        chain_attr("cy") != nullptr ? *chain_attr("cy") : std::string(), unit_h, 0.5);
    const double r =
        ParseGradientCoordinate(chain_attr("r") != nullptr ? *chain_attr("r") : std::string(),
                                (unit_w + unit_h) / 2.0,
                                0.5);
    const double fx = ParseGradientCoordinate(
        chain_attr("fx") != nullptr ? *chain_attr("fx") : std::string(), unit_w, cx);
    const double fy = ParseGradientCoordinate(
        chain_attr("fy") != nullptr ? *chain_attr("fy") : std::string(), unit_h, cy);
    const Point centre = to_device_point(cx, cy);
    const Point focus = to_device_point(fx, fy);
    gradient.cx = centre.x;
    gradient.cy = centre.y;
    gradient.fx = focus.x;
    gradient.fy = focus.y;
    // The radius is a length in gradient space: scale it by the composite
    // transform (mean scale; see AverageScale).
    const double local_scale = object_bounding_box ? (std::abs(bw) + std::abs(bh)) / 2.0
                                                   : AverageScale(gradient_transform);
    gradient.r = std::max(0.0, r * local_scale * AverageScale(to_device));
  }
  gradient.stops = std::move(stops);
  paint.has_gradient = true;
  paint.gradient = std::move(gradient);
  return paint;
}

// ---------------------------------------------------------------------------
// Rasterizer (2x supersampled RGBA accumulation)
// ---------------------------------------------------------------------------

struct RasterBuffer
{
  int width = 0;
  int height = 0;
  std::vector<double> r, g, b, a; // premultiplied color accumulation

  void Init(int w, int h)
  {
    width = w;
    height = h;
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    r.assign(n, 0);
    g.assign(n, 0);
    b.assign(n, 0);
    a.assign(n, 0);
  }

  void Blend(std::size_t idx, const Color& color, double coverage)
  {
    const double ca = color.a * coverage;
    r[idx] += color.r * ca;
    g[idx] += color.g * ca;
    b[idx] += color.b * ca;
    a[idx] += ca;
  }
};

void FillPolygon(RasterBuffer& buf, const std::vector<Point>& poly, const Paint& paint)
{
  if (poly.size() < 3) {
    return;
  }
  const Color color = paint.color;
  std::vector<double> xs;
  xs.reserve(poly.size());
  for (int y = 0; y < buf.height; ++y) {
    const double yc = static_cast<double>(y) + 0.5;
    xs.clear();
    for (std::size_t i = 0; i < poly.size(); ++i) {
      const Point& pa = poly[i];
      const Point& pb = poly[(i + 1) % poly.size()];
      if ((pa.y <= yc && pb.y > yc) || (pb.y <= yc && pa.y > yc)) {
        const double t = (yc - pa.y) / (pb.y - pa.y);
        xs.push_back(pa.x + t * (pb.x - pa.x));
      }
    }
    std::sort(xs.begin(), xs.end());
    for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
      double x0 = std::max(0.0, xs[i]);
      double x1 = std::min(static_cast<double>(buf.width), xs[i + 1]);
      if (x1 <= x0) {
        continue;
      }
      const int ix0 = static_cast<int>(x0);
      const int ix1 = std::min(buf.width, static_cast<int>(std::ceil(x1)));
      for (int x = ix0; x < ix1; ++x) {
        if (x < 0) {
          continue;
        }
        const double covered = std::min(
            1.0, std::min(static_cast<double>(x) + 1.0, x1) - std::max(static_cast<double>(x), x0));
        // Gradients are evaluated per device pixel; solid paints reuse |color|.
        buf.Blend(static_cast<std::size_t>(y) * static_cast<std::size_t>(buf.width) +
                      static_cast<std::size_t>(x),
                  paint.has_gradient
                      ? PaintAt(paint, static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5)
                      : color,
                  covered);
      }
    }
  }
}

void StrokeSegment(RasterBuffer& buf, Point p0, Point p1, double half_width, const Paint& paint)
{
  double dx = p1.x - p0.x;
  double dy = p1.y - p0.y;
  const double len = std::sqrt(dx * dx + dy * dy);
  if (len < 1e-9) {
    FillPolygon(buf,
                {{p0.x - half_width, p0.y - half_width},
                 {p0.x + half_width, p0.y - half_width},
                 {p0.x + half_width, p0.y + half_width},
                 {p0.x - half_width, p0.y + half_width}},
                paint);
    return;
  }
  dx /= len;
  dy /= len;
  const double nx = -dy * half_width;
  const double ny = dx * half_width;
  FillPolygon(buf,
              {{p0.x + nx, p0.y + ny},
               {p1.x + nx, p1.y + ny},
               {p1.x - nx, p1.y - ny},
               {p0.x - nx, p0.y - ny}},
              paint);
  auto cap = [&](Point c) {
    const int steps = 16;
    std::vector<Point> circle;
    circle.reserve(static_cast<std::size_t>(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
      const double a = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(steps);
      circle.push_back({c.x + half_width * std::cos(a), c.y + half_width * std::sin(a)});
    }
    FillPolygon(buf, circle, paint);
  };
  cap(p0);
  cap(p1);
}

void RenderShape(RasterBuffer& buf,
                 const Mat& transform,
                 const std::vector<Point>& points,
                 bool closed,
                 bool fill,
                 bool stroke,
                 double stroke_width,
                 const Paint& fill_paint,
                 const Paint& stroke_paint)
{
  if (points.empty()) {
    return;
  }
  std::vector<Point> device;
  device.reserve(points.size());
  for (const Point& p : points) {
    device.push_back(transform.Apply(p));
  }
  if (fill && device.size() >= 3) {
    FillPolygon(buf, device, fill_paint);
  }
  if (stroke && stroke_width > 0) {
    // stroke-width is in user units; the points were already transformed into
    // device space, so scale the width by the transform's average scale.
    const double scale = (std::sqrt(transform.a * transform.a + transform.b * transform.b) +
                          std::sqrt(transform.c * transform.c + transform.d * transform.d)) /
                         2.0;
    const double half = stroke_width / 2.0 * scale;
    for (std::size_t i = 0; i + 1 < device.size(); ++i) {
      StrokeSegment(buf, device[i], device[i + 1], half, stroke_paint);
    }
    if (closed && device.size() >= 3) {
      StrokeSegment(buf, device.back(), device.front(), half, stroke_paint);
    }
  }
}

// Bounding box ([x, y, w, h]) of a set of user-space polygons; empty input
// yields the unit square so that objectBoundingBox gradients stay well defined.
void BoundingBox(const std::vector<std::vector<Point>>& polygons, double* out)
{
  bool any = false;
  double min_x = 0, min_y = 0, max_x = 0, max_y = 0;
  for (const std::vector<Point>& poly : polygons) {
    for (const Point& p : poly) {
      if (!any) {
        min_x = max_x = p.x;
        min_y = max_y = p.y;
        any = true;
      } else {
        min_x = std::min(min_x, p.x);
        max_x = std::max(max_x, p.x);
        min_y = std::min(min_y, p.y);
        max_y = std::max(max_y, p.y);
      }
    }
  }
  if (!any) {
    out[0] = 0;
    out[1] = 0;
    out[2] = 1;
    out[3] = 1;
    return;
  }
  out[0] = min_x;
  out[1] = min_y;
  out[2] = std::max(0.0, max_x - min_x);
  out[3] = std::max(0.0, max_y - min_y);
}

struct PaintState
{
  bool has_fill = true;
  Color fill{0, 0, 0, 1};    // solid colour (or the fallback after url(#id))
  std::string fill_gradient; // url(#id) reference, empty for solid fills
  bool fill_none = false;    // url() without a fallback: paints nothing if unresolved
  double fill_alpha = 1.0;   // fill-opacity * opacity, also applied to gradients
  bool has_stroke = false;
  Color stroke{0, 0, 0, 1};
  std::string stroke_gradient;
  bool stroke_none = false;
  double stroke_alpha = 1.0;
  double stroke_width = 1;
};

// Flattens one path (a sequence of curves/arcs) into closed/open polygons.
// The whole element is flattened before drawing so that objectBoundingBox
// gradients can use the path's bounding box.
void RenderPath(RasterBuffer& buf,
                const Mat& transform,
                std::string_view d,
                bool fill,
                bool stroke,
                double stroke_width,
                const PaintState& state,
                const GradientDefs& defs,
                double viewport_w,
                double viewport_h)
{
  const std::vector<PathCmd> commands = ParsePathData(d);
  Point current{0, 0};
  Point start{0, 0};
  std::vector<std::vector<Point>> subpaths;
  std::vector<bool> closed;

  auto begin_subpath = [&](const Point& p) {
    subpaths.push_back({p});
    closed.push_back(false);
  };
  auto add_point = [&](const Point& p) {
    if (subpaths.empty()) {
      begin_subpath(p);
    } else {
      subpaths.back().push_back(p);
    }
    current = p;
  };
  auto add_flat = [&](const std::vector<Point>& flat) {
    for (const Point& p : flat) {
      add_point(p);
    }
  };
  auto close_subpath = [&]() {
    if (!subpaths.empty() && subpaths.back().size() >= 2) {
      closed.back() = true;
    }
    subpaths.push_back({start});
    closed.push_back(false);
    current = start;
  };
  const auto last_point = [&]() -> Point {
    return subpaths.empty() || subpaths.back().size() < 2
               ? current
               : subpaths.back()[subpaths.back().size() - 2];
  };

  for (const PathCmd& cmd : commands) {
    const std::vector<double>& a = cmd.args;
    switch (cmd.cmd) {
    case 'M':
      current = {a[0], a[1]};
      start = current;
      // Further coordinate pairs after M are implicit L (SVG 1.1 §8.3.2).
      begin_subpath(current);
      for (std::size_t k = 2; k + 1 < a.size(); k += 2) {
        add_point({a[k], a[k + 1]});
      }
      break;
    case 'L':
      for (std::size_t k = 0; k + 1 < a.size(); k += 2) {
        add_point({a[k], a[k + 1]});
      }
      break;
    case 'H':
      for (const double x : a) {
        add_point({x, current.y});
      }
      break;
    case 'V':
      for (const double y : a) {
        add_point({current.x, y});
      }
      break;
    case 'C':
      for (std::size_t k = 0; k + 5 < a.size(); k += 6) {
        const Point p0 = current;
        const Point p1{a[k], a[k + 1]};
        const Point p2{a[k + 2], a[k + 3]};
        const Point p3{a[k + 4], a[k + 5]};
        std::vector<Point> flat;
        FlattenCubic(flat, p0, p1, p2, p3, 8);
        add_flat(flat);
      }
      break;
    case 'S':
      for (std::size_t k = 0; k + 3 < a.size(); k += 4) {
        const Point p0 = current;
        const Point p1 = last_point();
        const Point p2{a[k], a[k + 1]};
        const Point p3{a[k + 2], a[k + 3]};
        std::vector<Point> flat;
        FlattenCubic(flat, p0, p1, p2, p3, 8);
        add_flat(flat);
      }
      break;
    case 'Q':
      for (std::size_t k = 0; k + 3 < a.size(); k += 4) {
        const Point p0 = current;
        const Point p1{a[k], a[k + 1]};
        const Point p2{a[k + 2], a[k + 3]};
        std::vector<Point> flat;
        FlattenQuadratic(flat, p0, p1, p2, 8);
        add_flat(flat);
      }
      break;
    case 'T':
      for (std::size_t k = 0; k + 1 < a.size(); k += 2) {
        const Point p0 = current;
        const Point p1 = last_point();
        const Point p2{a[k], a[k + 1]};
        std::vector<Point> flat;
        FlattenQuadratic(flat, p0, p1, p2, 8);
        add_flat(flat);
      }
      break;
    case 'A':
      for (std::size_t k = 0; k + 6 < a.size(); k += 7) {
        const Point p0 = current;
        const Point p1{a[k + 5], a[k + 6]};
        std::vector<Point> flat;
        FlattenArc(flat, p0, a[k], a[k + 1], a[k + 2], a[k + 3] != 0, a[k + 4] != 0, p1);
        add_flat(flat);
      }
      break;
    case 'Z':
      close_subpath();
      break;
    default:
      break;
    }
  }
  double bbox[4] = {0, 0, 1, 1};
  BoundingBox(subpaths, bbox);
  const Paint fill_paint = MakePaint(state.fill,
                                     state.fill_gradient,
                                     state.fill_none,
                                     state.fill_alpha,
                                     defs,
                                     bbox,
                                     viewport_w,
                                     viewport_h,
                                     transform);
  const Paint stroke_paint = MakePaint(state.stroke,
                                       state.stroke_gradient,
                                       state.stroke_none,
                                       state.stroke_alpha,
                                       defs,
                                       bbox,
                                       viewport_w,
                                       viewport_h,
                                       transform);
  for (std::size_t i = 0; i < subpaths.size(); ++i) {
    RenderShape(buf,
                transform,
                subpaths[i],
                closed[i],
                fill,
                stroke,
                stroke_width,
                fill_paint,
                stroke_paint);
  }
}

// Extracts the id out of a url(#id) paint reference; returns false when the
// value is not a URL reference.  A CSS fallback colour after the reference
// ("url(#g) red") is parsed into |fallback|.
bool ParsePaintUrl(std::string_view value, std::string& id, std::string& fallback)
{
  std::string s(value);
  const std::size_t open = s.find("url(");
  if (open == std::string::npos) {
    return false;
  }
  std::size_t i = open + 4;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
    ++i;
  }
  std::string ref;
  if (i < s.size() && (s[i] == '#' || s[i] == '"' || s[i] == '\'')) {
    const char quote = s[i] == '#' ? '\0' : s[i];
    if (quote != '\0') {
      ++i;
    }
    const std::size_t start = i;
    while (i < s.size() && ((quote == '\0' && s[i] != ')') || (quote != '\0' && s[i] != quote))) {
      ++i;
    }
    ref = s.substr(start, i - start);
  }
  const std::size_t close = s.find(')', i);
  if (close == std::string::npos) {
    return false;
  }
  while (!ref.empty() && std::isspace(static_cast<unsigned char>(ref.back()))) {
    ref.pop_back();
  }
  if (ref.empty() || ref[0] != '#') {
    return false;
  }
  id = ref.substr(1);
  fallback = s.substr(close + 1);
  // Trim the fallback.
  std::size_t begin = 0;
  while (begin < fallback.size() && std::isspace(static_cast<unsigned char>(fallback[begin]))) {
    ++begin;
  }
  fallback = fallback.substr(begin);
  return true;
}

void ApplyPaintAttr(PaintState& state, std::string_view attr, std::string_view value)
{
  // Paint references ("url(#id)") are resolved at draw time, when the painted
  // shape's bounding box is known.  An optional fallback colour follows the
  // reference (CSS <paint> syntax).
  if (attr == "fill" || attr == "stroke") {
    std::string id;
    std::string fallback;
    if (ParsePaintUrl(value, id, fallback)) {
      // An unresolvable reference paints nothing (SVG 1.1 §13.2.2); a CSS
      // fallback colour after the reference is used instead when present.
      if (attr == "fill") {
        state.has_fill = true;
        state.fill_gradient = id;
        state.fill_none = fallback.empty();
      } else {
        state.has_stroke = true;
        state.stroke_gradient = id;
        state.stroke_none = fallback.empty();
      }
      if (fallback.empty()) {
        return;
      }
      value = fallback; // fall through to parse the fallback colour
    } else {
      if (attr == "fill") {
        state.fill_gradient.clear();
        state.fill_none = false;
      } else {
        state.stroke_gradient.clear();
        state.stroke_none = false;
      }
    }
  }
  Color color;
  bool none = false;
  const bool ok = ParseColor(value, color, none);
  if (attr == "fill") {
    if (none) {
      state.has_fill = false;
    } else if (ok) {
      state.has_fill = true;
      state.fill = color;
    }
  } else if (attr == "stroke") {
    if (none) {
      state.has_stroke = false;
    } else if (ok) {
      state.has_stroke = true;
      state.stroke = color;
    }
  } else if (attr == "stroke-width") {
    state.stroke_width = std::max(0.0, std::strtod(std::string(value).c_str(), nullptr));
  } else if (attr == "fill-opacity") {
    const double o = std::max(0.0, std::min(1.0, std::strtod(std::string(value).c_str(), nullptr)));
    state.fill.a = o;
    state.fill_alpha = o;
  } else if (attr == "stroke-opacity") {
    const double o = std::max(0.0, std::min(1.0, std::strtod(std::string(value).c_str(), nullptr)));
    state.stroke.a = o;
    state.stroke_alpha = o;
  } else if (attr == "opacity") {
    const double o = std::max(0.0, std::min(1.0, std::strtod(std::string(value).c_str(), nullptr)));
    state.fill.a *= o;
    state.stroke.a *= o;
    state.fill_alpha *= o;
    state.stroke_alpha *= o;
  }
}

// Applies the declarations of a style="a:b;c:d" attribute (the SVG/CSS style
// attribute outranks presentation attributes on the same element).
void ApplyStyleDeclarations(PaintState& state, std::string_view style)
{
  std::size_t pos = 0;
  while (pos < style.size()) {
    const std::size_t end = style.find(';', pos);
    const std::string_view decl =
        style.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
    const std::size_t colon = decl.find(':');
    if (colon != std::string_view::npos) {
      std::string name(ToLower(decl.substr(0, colon)));
      std::string_view value = decl.substr(colon + 1);
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
      }
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
      }
      if (name == "fill" || name == "stroke" || name == "stroke-width" || name == "fill-opacity" ||
          name == "stroke-opacity" || name == "opacity") {
        ApplyPaintAttr(state, name, value);
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    pos = end + 1;
  }
}

// ---------------------------------------------------------------------------
// Text (<text>, <tspan>) through an injected glyph-outline shaper
// ---------------------------------------------------------------------------

// Fills a set of closed contours with the even-odd rule, which is what glyph
// outlines need: the inner contour of "o" subtracts the counter.  Edges are
// wrapped within each contour only (never between contours).
void FillContours(RasterBuffer& buf,
                  const std::vector<std::vector<Point>>& contours,
                  const Paint& paint)
{
  if (contours.empty()) {
    return;
  }
  const Color color = paint.color;
  std::vector<double> xs;
  for (int y = 0; y < buf.height; ++y) {
    const double yc = static_cast<double>(y) + 0.5;
    xs.clear();
    for (const std::vector<Point>& contour : contours) {
      if (contour.size() < 2) {
        continue;
      }
      for (std::size_t i = 0; i < contour.size(); ++i) {
        const Point& pa = contour[i];
        const Point& pb = contour[(i + 1) % contour.size()];
        if ((pa.y <= yc && pb.y > yc) || (pb.y <= yc && pa.y > yc)) {
          const double t = (yc - pa.y) / (pb.y - pa.y);
          xs.push_back(pa.x + t * (pb.x - pa.x));
        }
      }
    }
    std::sort(xs.begin(), xs.end());
    for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
      const double x0 = std::max(0.0, xs[i]);
      const double x1 = std::min(static_cast<double>(buf.width), xs[i + 1]);
      if (x1 <= x0) {
        continue;
      }
      const int ix0 = static_cast<int>(x0);
      const int ix1 = std::min(buf.width, static_cast<int>(std::ceil(x1)));
      for (int x = ix0; x < ix1; ++x) {
        if (x < 0) {
          continue;
        }
        const double covered = std::min(
            1.0, std::min(static_cast<double>(x) + 1.0, x1) - std::max(static_cast<double>(x), x0));
        buf.Blend(static_cast<std::size_t>(y) * static_cast<std::size_t>(buf.width) +
                      static_cast<std::size_t>(x),
                  paint.has_gradient
                      ? PaintAt(paint, static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5)
                      : color,
                  covered);
      }
    }
  }
}

struct TextStyle
{
  double font_size = 16; // SVG's initial font-size
  std::string family = "sans-serif";
  bool bold = false;
  bool italic = false;
  double letter_spacing = 0;
  int anchor = 0; // 0 = start, 1 = middle, 2 = end
};

// Trims leading/trailing whitespace and collapses runs of spaces+newlines into
// a single space (SVG's default xml:space="default" handling).
std::string CollapseWhitespace(std::string_view text)
{
  std::string out;
  bool pending_space = false;
  for (const char c : text) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    out.push_back(c);
  }
  return out;
}

double ParseFontSize(std::string_view value, double parent_size)
{
  std::string s(value);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.erase(s.begin());
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  if (s.empty()) {
    return parent_size;
  }
  // Named absolute sizes (CSS 2.1 font-size keywords).
  static const std::pair<const char*, double> kNamed[] = {{"xx-small", 9},
                                                          {"x-small", 10},
                                                          {"small", 13},
                                                          {"medium", 16},
                                                          {"large", 18},
                                                          {"x-large", 24},
                                                          {"xx-large", 32},
                                                          {"smaller", 13},
                                                          {"larger", 19},
                                                          {"inherit", -1}};
  for (const auto& [name, size] : kNamed) {
    if (s == name) {
      return size < 0 ? parent_size : size;
    }
  }
  const bool percent = s.back() == '%';
  if (percent || (s.size() >= 2 && s.substr(s.size() - 2) == "em")) {
    const std::string number = percent ? s.substr(0, s.size() - 1) : s.substr(0, s.size() - 2);
    const double factor = std::strtod(number.c_str(), nullptr);
    return factor > 0 ? parent_size * (percent ? factor / 100.0 : factor) : parent_size;
  }
  const double px = std::strtod(s.c_str(), nullptr);
  return px > 0 ? px : parent_size;
}

void ApplyTextAttr(TextStyle& style, std::string_view attr, std::string_view value)
{
  if (attr == "font-size") {
    style.font_size = ParseFontSize(value, style.font_size);
  } else if (attr == "font-family") {
    // Keep the author's list; the shaper resolves it (quotes are stripped).
    std::string family(value);
    if (family.size() >= 2 && (family.front() == '\'' || family.front() == '"')) {
      family = family.substr(1, family.size() - 2);
    }
    if (!family.empty()) {
      style.family = family;
    }
  } else if (attr == "font-weight") {
    const std::string v = ToLower(value);
    style.bold = v == "bold" || v == "bolder" || std::strtod(v.c_str(), nullptr) >= 600;
  } else if (attr == "font-style") {
    const std::string v = ToLower(value);
    style.italic = v == "italic" || v == "oblique";
  } else if (attr == "letter-spacing") {
    const std::string v = ToLower(value);
    style.letter_spacing = v == "normal" ? 0.0 : std::strtod(v.c_str(), nullptr);
  } else if (attr == "text-anchor") {
    const std::string v = ToLower(value);
    style.anchor = v == "middle" ? 1 : (v == "end" ? 2 : 0);
  }
}

// Shapes one string and appends its flattened outlines (in *user* space, after
// |transform|) to |contours|.  Returns the advance width in user units.
double ShapeRun(const SvgTextShaper& shaper,
                const TextStyle& style,
                std::string_view text,
                double pen_x,
                double pen_y,
                const Mat& transform,
                std::vector<std::vector<Point>>& contours)
{
  std::vector<SvgGlyphOutline> glyphs;
  if (!shaper(style.family, style.bold, style.italic, style.font_size, text, glyphs)) {
    return 0;
  }
  double pen = pen_x;
  for (const SvgGlyphOutline& glyph : glyphs) {
    std::vector<Point> current;
    Point cursor{0, 0};
    const auto flush = [&]() {
      if (current.size() >= 3) {
        std::vector<Point> device;
        device.reserve(current.size());
        for (const Point& p : current) {
          device.push_back(transform.Apply({p.x + pen, p.y + pen_y}));
        }
        contours.push_back(std::move(device));
      }
      current.clear();
    };
    for (const SvgOutlineEdge& edge : glyph.edges) {
      switch (edge.kind) {
      case SvgOutlineEdge::kMove:
        flush();
        cursor = {edge.p[0], edge.p[1]};
        current.push_back(cursor);
        break;
      case SvgOutlineEdge::kLine:
        cursor = {edge.p[0], edge.p[1]};
        current.push_back(cursor);
        break;
      case SvgOutlineEdge::kQuadratic: {
        const Point control{edge.p[0], edge.p[1]};
        const Point end{edge.p[2], edge.p[3]};
        FlattenQuadratic(current, cursor, control, end, 6);
        cursor = end;
        break;
      }
      case SvgOutlineEdge::kCubic: {
        const Point c1{edge.p[0], edge.p[1]};
        const Point c2{edge.p[2], edge.p[3]};
        const Point end{edge.p[4], edge.p[5]};
        FlattenCubic(current, cursor, c1, c2, end, 6);
        cursor = end;
        break;
      }
      case SvgOutlineEdge::kClose:
        flush();
        break;
      }
    }
    flush();
    pen += glyph.advance + style.letter_spacing;
  }
  return pen - pen_x;
}

struct TextCursor
{
  double x = 0;
  double y = 0;
};

void RenderTextNode(RasterBuffer& buf,
                    const Element& element,
                    const Mat& transform,
                    const PaintState& parent_state,
                    const GradientDefs& defs,
                    double viewport_w,
                    double viewport_h,
                    const SvgTextShaper& shaper,
                    TextStyle style,
                    TextCursor cursor,
                    bool apply_anchor);

// Renders one shaped string at |cursor| and advances it (used for both direct
// character data and the string inside a <tspan>).
void RenderTextString(RasterBuffer& buf,
                      std::string_view raw_text,
                      const Mat& transform,
                      const PaintState& state,
                      const GradientDefs& defs,
                      double viewport_w,
                      double viewport_h,
                      const SvgTextShaper& shaper,
                      const TextStyle& style,
                      TextCursor& cursor,
                      bool apply_anchor)
{
  const std::string text = CollapseWhitespace(raw_text);
  if (text.empty()) {
    return;
  }
  // Measure first: text-anchor shifts the run by its own width.
  std::vector<SvgGlyphOutline> glyphs;
  if (!shaper(style.family, style.bold, style.italic, style.font_size, text, glyphs)) {
    return;
  }
  double width = 0;
  for (const SvgGlyphOutline& glyph : glyphs) {
    width += glyph.advance + style.letter_spacing;
  }
  if (apply_anchor && style.anchor == 1) {
    cursor.x -= width / 2.0;
  } else if (apply_anchor && style.anchor == 2) {
    cursor.x -= width;
  }
  std::vector<std::vector<Point>> contours;
  ShapeRun(shaper, style, text, cursor.x, cursor.y, transform, contours);
  cursor.x += width;
  if (contours.empty()) {
    return;
  }
  // objectBoundingBox gradients use the text run's bounding box (SVG 1.1
  // §13.2.2).  The contours are already in device space, so the box must be
  // mapped back through the inverse transform -- instead, compute the box from
  // the device contours and let MakePaint work with a device-space box by
  // passing the identity transform.
  double bbox[4] = {0, 0, 0, 0};
  BoundingBox(contours, bbox);
  const Paint fill_paint = MakePaint(state.fill,
                                     state.fill_gradient,
                                     state.fill_none,
                                     state.fill_alpha,
                                     defs,
                                     nullptr,
                                     viewport_w,
                                     viewport_h,
                                     Mat{});
  Paint paint = fill_paint;
  if (paint.has_gradient) {
    // Rebuild the gradient so its coordinates follow the device-space box.
    paint = MakePaint(state.fill,
                      state.fill_gradient,
                      state.fill_none,
                      state.fill_alpha,
                      defs,
                      bbox,
                      0,
                      0,
                      Mat{});
  }
  if (state.has_fill) {
    FillContours(buf, contours, paint);
  }
}

void RenderTextNode(RasterBuffer& buf,
                    const Element& element,
                    const Mat& transform,
                    const PaintState& parent_state,
                    const GradientDefs& defs,
                    double viewport_w,
                    double viewport_h,
                    const SvgTextShaper& shaper,
                    TextStyle style,
                    TextCursor cursor,
                    bool apply_anchor)
{
  PaintState state = parent_state;
  for (const auto& attr : element.attrs) {
    if (attr.first == "fill" || attr.first == "stroke" || attr.first == "stroke-width" ||
        attr.first == "fill-opacity" || attr.first == "stroke-opacity" || attr.first == "opacity") {
      ApplyPaintAttr(state, attr.first, attr.second);
    } else if (attr.first == "font-size" || attr.first == "font-family" ||
               attr.first == "font-weight" || attr.first == "font-style" ||
               attr.first == "letter-spacing" || attr.first == "text-anchor" ||
               attr.first == "xml:space") {
      ApplyTextAttr(style, attr.first, attr.second);
    }
  }
  if (const std::string* style_attr = element.Attr("style")) {
    ApplyStyleDeclarations(state, *style_attr);
    std::size_t pos = 0;
    while (pos < style_attr->size()) {
      const std::size_t end = style_attr->find(';', pos);
      const std::string_view decl =
          std::string_view(*style_attr)
              .substr(pos, end == std::string::npos ? std::string::npos : end - pos);
      const std::size_t colon = decl.find(':');
      if (colon != std::string_view::npos) {
        std::string name(ToLower(decl.substr(0, colon)));
        std::string_view value = decl.substr(colon + 1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
          value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
          value.remove_suffix(1);
        }
        ApplyTextAttr(style, name, value);
      }
      if (end == std::string::npos) {
        break;
      }
      pos = end + 1;
    }
  }

  // Position: absolute x/y replace the current pen, dx/dy offset it.
  bool explicit_position = false;
  if (const std::string* x = element.Attr("x")) {
    cursor.x = ParseGradientCoordinate(*x, viewport_w, cursor.x);
    explicit_position = true;
  }
  if (const std::string* y = element.Attr("y")) {
    cursor.y = ParseGradientCoordinate(*y, viewport_h, cursor.y);
    explicit_position = true;
  }
  if (const std::string* dx = element.Attr("dx")) {
    cursor.x += ParseGradientCoordinate(*dx, viewport_w, 0.0);
  }
  if (const std::string* dy = element.Attr("dy")) {
    cursor.y += ParseGradientCoordinate(*dy, viewport_h, 0.0);
  }

  // Characters and child elements are rendered in document order.
  for (const Element& child : element.children) {
    if (child.name == "#text") {
      RenderTextString(buf,
                       child.text,
                       transform,
                       state,
                       defs,
                       viewport_w,
                       viewport_h,
                       shaper,
                       style,
                       cursor,
                       apply_anchor && explicit_position);
      continue;
    }
    if (child.name == "tspan") {
      RenderTextNode(buf,
                     child,
                     transform,
                     state,
                     defs,
                     viewport_w,
                     viewport_h,
                     shaper,
                     style,
                     cursor,
                     /*apply_anchor=*/true);
      continue;
    }
    // Unknown child elements inside <text> are ignored (also true for the
    // browser: they simply contribute nothing to the run).
  }
}

// ---------------------------------------------------------------------------
// Tree rendering
// ---------------------------------------------------------------------------

double AttrNumber(const Element& element, std::string_view key, double dflt)
{
  const std::string* v = element.Attr(key);
  return v == nullptr || v->empty() ? dflt : std::strtod(v->c_str(), nullptr);
}

// |viewport_w| / |viewport_h| are the current user-space viewport dimensions
// (needed to resolve percentage coordinates of userSpaceOnUse gradients).
void RenderElement(RasterBuffer& buf,
                   const Element& element,
                   const Mat& parent,
                   PaintState state,
                   const GradientDefs& defs,
                   double viewport_w,
                   double viewport_h,
                   const SvgTextShaper& shaper)
{
  for (const auto& attr : element.attrs) {
    if (attr.first == "fill" || attr.first == "stroke" || attr.first == "stroke-width" ||
        attr.first == "fill-opacity" || attr.first == "stroke-opacity" || attr.first == "opacity") {
      ApplyPaintAttr(state, attr.first, attr.second);
    }
  }
  if (const std::string* style = element.Attr("style")) {
    ApplyStyleDeclarations(state, *style);
  }
  Mat local = parent;
  if (const std::string* t = element.Attr("transform")) {
    Mat m;
    if (ParseTransform(*t, m)) {
      local = parent * m;
    }
  }
  // Paint factories: a gradient is built lazily per shape because
  // objectBoundingBox coordinates depend on that shape's bounding box.
  const auto fill_paint = [&](const double* bbox) {
    return MakePaint(state.fill,
                     state.fill_gradient,
                     state.fill_none,
                     state.fill_alpha,
                     defs,
                     bbox,
                     viewport_w,
                     viewport_h,
                     local);
  };
  const auto stroke_paint = [&](const double* bbox) {
    return MakePaint(state.stroke,
                     state.stroke_gradient,
                     state.stroke_none,
                     state.stroke_alpha,
                     defs,
                     bbox,
                     viewport_w,
                     viewport_h,
                     local);
  };

  if (element.name == "svg" || element.name == "g" || element.name == "a") {
    double vw = viewport_w;
    double vh = viewport_h;
    if (element.name == "svg") {
      // A nested <svg> re-establishes the viewport for percentage lengths.
      if (const std::string* w = element.Attr("width")) {
        vw = std::strtod(w->c_str(), nullptr);
      }
      if (const std::string* h = element.Attr("height")) {
        vh = std::strtod(h->c_str(), nullptr);
      }
    }
    for (const Element& child : element.children) {
      RenderElement(buf, child, local, state, defs, vw, vh, shaper);
    }
    return;
  }
  if (element.name == "rect") {
    const double x = AttrNumber(element, "x", 0);
    const double y = AttrNumber(element, "y", 0);
    const double w = AttrNumber(element, "width", 0);
    const double h = AttrNumber(element, "height", 0);
    const double rx = AttrNumber(element, "rx", 0);
    const double ry = AttrNumber(element, "ry", rx);
    if (rx > 0 || ry > 0) {
      const double rxx = std::min(rx, w / 2);
      const double ryy = std::min(ry, h / 2);
      std::vector<Point> smooth;
      smooth.reserve(24);
      const Point c0{x + rxx, y};
      const Point c1{x + w - rxx, y};
      const Point c2{x + w, y + ryy};
      const Point c3{x + w, y + h - ryy};
      const Point c4{x + w - rxx, y + h};
      const Point c5{x + rxx, y + h};
      const Point c6{x, y + h - ryy};
      const Point c7{x, y + ryy};
      smooth.push_back(c0);
      FlattenCubic(smooth, c0, {x + rxx / 2, y}, {x, y + ryy / 2}, c7, 6);
      smooth.push_back(c1);
      FlattenCubic(smooth, c1, {x + w - rxx / 2, y}, {x + w, y + ryy / 2}, c2, 6);
      smooth.push_back(c3);
      FlattenCubic(smooth, c3, {x + w, y + h - ryy / 2}, {x + w - rxx / 2, y + h}, c4, 6);
      smooth.push_back(c5);
      FlattenCubic(smooth, c5, {x + rxx / 2, y + h}, {x, y + h - ryy / 2}, c6, 6);
      const double bbox[4] = {x, y, w, h};
      RenderShape(buf,
                  local,
                  smooth,
                  true,
                  state.has_fill,
                  state.has_stroke,
                  state.stroke_width,
                  fill_paint(bbox),
                  stroke_paint(bbox));
      return;
    }
    const double bbox[4] = {x, y, w, h};
    RenderShape(buf,
                local,
                {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}},
                true,
                state.has_fill,
                state.has_stroke,
                state.stroke_width,
                fill_paint(bbox),
                stroke_paint(bbox));
    return;
  }
  if (element.name == "circle" || element.name == "ellipse") {
    const double cx = AttrNumber(element, "cx", 0);
    const double cy = AttrNumber(element, "cy", 0);
    const double rx =
        element.name == "ellipse" ? AttrNumber(element, "rx", 0) : AttrNumber(element, "r", 0);
    const double ry = element.name == "ellipse" ? AttrNumber(element, "ry", 0) : rx;
    std::vector<Point> pts;
    const int steps = 64;
    pts.reserve(static_cast<std::size_t>(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
      const double a = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(steps);
      pts.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
    }
    const double bbox[4] = {cx - rx, cy - ry, 2 * rx, 2 * ry};
    RenderShape(buf,
                local,
                pts,
                true,
                state.has_fill,
                state.has_stroke,
                state.stroke_width,
                fill_paint(bbox),
                stroke_paint(bbox));
    return;
  }
  if (element.name == "line") {
    const Point p1{AttrNumber(element, "x1", 0), AttrNumber(element, "y1", 0)};
    const Point p2{AttrNumber(element, "x2", 0), AttrNumber(element, "y2", 0)};
    const double bbox[4] = {
        std::min(p1.x, p2.x), std::min(p1.y, p2.y), std::abs(p2.x - p1.x), std::abs(p2.y - p1.y)};
    RenderShape(buf,
                local,
                {p1, p2},
                false,
                false,
                state.has_stroke,
                state.stroke_width,
                fill_paint(bbox),
                stroke_paint(bbox));
    return;
  }
  if (element.name == "polyline" || element.name == "polygon") {
    std::vector<Point> pts;
    const std::string s = element.AttrOr("points", "");
    std::size_t pos = 0;
    while (pos < s.size()) {
      double x, y;
      if (!ParseNumber(s, pos, x) || !ParseNumber(s, pos, y)) {
        break;
      }
      pts.push_back({x, y});
    }
    double bbox[4] = {0, 0, 1, 1};
    BoundingBox({pts}, bbox);
    RenderShape(buf,
                local,
                pts,
                element.name == "polygon",
                state.has_fill,
                state.has_stroke,
                state.stroke_width,
                fill_paint(bbox),
                stroke_paint(bbox));
    return;
  }
  if (element.name == "path") {
    if (const std::string* d = element.Attr("d")) {
      RenderPath(buf,
                 local,
                 *d,
                 state.has_fill,
                 state.has_stroke,
                 state.stroke_width,
                 state,
                 defs,
                 viewport_w,
                 viewport_h);
    }
    return;
  }
  if (element.name == "text") {
    if (!shaper) {
      return; // no glyph provider: text is skipped, never faked
    }
    TextStyle style;
    TextCursor cursor;
    RenderTextNode(buf,
                   element,
                   local,
                   state,
                   defs,
                   viewport_w,
                   viewport_h,
                   shaper,
                   style,
                   cursor,
                   /*apply_anchor=*/true);
    return;
  }
  if (element.name == "defs" || element.name == "symbol" || element.name == "marker" ||
      element.name == "clippath" || element.name == "mask" || element.name == "pattern" ||
      element.name == "lineargradient" || element.name == "radialgradient" ||
      element.name == "style" || element.name == "title" || element.name == "desc" ||
      element.name == "#text") {
    return; // skipped content
  }
  for (const Element& child : element.children) {
    RenderElement(buf, child, local, state, defs, viewport_w, viewport_h, shaper);
  }
}

} // namespace

bool IsSvg(std::string_view data)
{
  const std::size_t pos = SkipXmlProlog(data, 0);
  return pos + 4 <= data.size() && data.substr(pos, 4) == "<svg";
}

base::Result<Image> DecodeSvg(std::string_view data, const SvgTextShaper& shaper)
{
  std::size_t pos = SkipXmlProlog(data, 0);
  Element root;
  std::size_t end = pos;
  if (!ParseElement(data, pos, root, end) || root.name != "svg") {
    return base::Err(base::Error::Parse("not an SVG document"));
  }

  double width = 300;
  double height = 150;
  bool has_viewbox = false;
  double vbx = 0, vby = 0, vbw = 0, vbh = 0;
  if (const std::string* w = root.Attr("width")) {
    width = std::strtod(w->c_str(), nullptr);
  }
  if (const std::string* h = root.Attr("height")) {
    height = std::strtod(h->c_str(), nullptr);
  }
  if (const std::string* vb = root.Attr("viewBox")) {
    std::string s(*vb);
    std::size_t p = 0;
    double vals[4] = {0, 0, 0, 0};
    int n = 0;
    while (n < 4) {
      double v;
      if (!ParseNumber(s, p, v)) {
        break;
      }
      vals[n++] = v;
    }
    if (n == 4) {
      has_viewbox = true;
      vbx = vals[0];
      vby = vals[1];
      vbw = vals[2];
      vbh = vals[3];
    }
  }

  if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
      width > 4096 || height > 4096) {
    return base::Err(base::Error::Parse("invalid SVG dimensions"));
  }
  const int out_w = static_cast<int>(std::ceil(width));
  const int out_h = static_cast<int>(std::ceil(height));
  RasterBuffer buf;
  buf.Init(out_w * 2, out_h * 2);

  Mat to_device = Mat::Scale(2.0, 2.0);
  if (has_viewbox && std::isfinite(vbx) && std::isfinite(vby) && std::isfinite(vbw) &&
      std::isfinite(vbh) && vbw > 0 && vbh > 0) {
    const double scale =
        std::min(static_cast<double>(buf.width) / vbw, static_cast<double>(buf.height) / vbh);
    const double tx = (static_cast<double>(buf.width) - vbw * scale) / 2.0 - vbx * scale;
    const double ty = (static_cast<double>(buf.height) - vbh * scale) / 2.0 - vby * scale;
    to_device = Mat::Translate(tx, ty) * Mat::Scale(scale, scale);
  } else if (has_viewbox) {
    to_device = Mat::Translate(-vbx * 2.0, -vby * 2.0) * Mat::Scale(2.0, 2.0);
  }

  // Gradient definitions may live anywhere in the document (including after the
  // shapes that reference them), so collect them before rendering.
  GradientDefs defs;
  CollectGradientDefs(root, defs);

  // User-space viewport for percentage lengths inside userSpaceOnUse gradients.
  const double viewport_w = has_viewbox && vbw > 0 ? vbw : width;
  const double viewport_h = has_viewbox && vbh > 0 ? vbh : height;

  PaintState state;
  RenderElement(buf, root, to_device, state, defs, viewport_w, viewport_h, shaper);

  // Downsample 2x2 and unpremultiply.
  Image image;
  image.width = out_w;
  image.height = out_h;
  image.rgba.assign(static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h) * 4, 0);
  for (int y = 0; y < out_h; ++y) {
    for (int x = 0; x < out_w; ++x) {
      double ar = 0, ag = 0, ab = 0, aa = 0;
      for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
          const std::size_t idx =
              static_cast<std::size_t>(y * 2 + dy) * static_cast<std::size_t>(buf.width) +
              static_cast<std::size_t>(x * 2 + dx);
          ar += buf.r[idx];
          ag += buf.g[idx];
          ab += buf.b[idx];
          aa += buf.a[idx];
        }
      }
      // |ar| and |aa| are both sums over the same four samples, so the
      // unpremultiplied colour is their ratio (dividing by 4 twice cancels).
      // Using 4/aa here made every non-saturated colour ~4x too bright.
      const double inv = aa > 0 ? 1.0 / aa : 0;
      const std::size_t idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(out_w) +
                               static_cast<std::size_t>(x)) *
                              4;
      const double alpha = std::min(1.0, aa / 4.0);
      image.rgba[idx + 0] = static_cast<uint8_t>(std::min(255.0, ar * inv) + 0.5);
      image.rgba[idx + 1] = static_cast<uint8_t>(std::min(255.0, ag * inv) + 0.5);
      image.rgba[idx + 2] = static_cast<uint8_t>(std::min(255.0, ab * inv) + 0.5);
      image.rgba[idx + 3] = static_cast<uint8_t>(alpha * 255.0 + 0.5);
    }
  }
  return base::Ok(std::move(image));
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

} // namespace neko::image
