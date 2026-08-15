// Minimal SVG rasterizer.
//
// Scope (see svg_decoder.h): shapes, fill/stroke, transforms, viewBox scaling.
// The XML parsing here is intentionally minimal — elements + attributes only,
// enough for the SVG constructs pages actually use in <img> logos and icons.

#include "neko/image/svg_decoder.h"

#include "neko/base/status.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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

  static Mat Translate(double tx, double ty) { return {1, 0, 0, 1, tx, ty}; }
  static Mat Scale(double sx, double sy) { return {sx, 0, 0, sy, 0, 0}; }
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
  Point Apply(const Point& p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
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
      {"black", 0, 0, 0},        {"white", 255, 255, 255}, {"red", 255, 0, 0},
      {"green", 0, 128, 0},      {"lime", 0, 255, 0},      {"blue", 0, 0, 255},
      {"yellow", 255, 255, 0},   {"cyan", 0, 255, 255},    {"magenta", 255, 0, 255},
      {"gray", 128, 128, 128},   {"grey", 128, 128, 128},  {"silver", 192, 192, 192},
      {"maroon", 128, 0, 0},     {"olive", 128, 128, 0},   {"purple", 128, 0, 128},
      {"teal", 0, 128, 128},     {"navy", 0, 0, 128},      {"orange", 255, 165, 0},
      {"brown", 165, 42, 42},    {"pink", 255, 192, 203},  {"gold", 255, 215, 0},
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
    if (hex.size() == 6 && ParseHexByte(hex.substr(0, 2), r) &&
        ParseHexByte(hex.substr(2, 2), g) && ParseHexByte(hex.substr(4, 2), b)) {
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
    while (i < data.size() && data[i] != '<') {
      ++i;
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
      v = args[k];
      if (relative && (abs_cmd == 'M' || abs_cmd == 'L' || abs_cmd == 'H' || abs_cmd == 'V' ||
                       abs_cmd == 'C' || abs_cmd == 'S' || abs_cmd == 'Q' || abs_cmd == 'T' ||
                       abs_cmd == 'A')) {
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
        PathCmd c;
        c.cmd = 'C';
        for (std::size_t k = 0; k + 5 < args.size(); k += 6) {
          double c1x, c1y, c2x, c2y, x, y;
          num(k, c1x);
          num(k + 1, c1y);
          num(k + 2, c2x);
          num(k + 3, c2y);
          num(k + 4, x);
          num(k + 5, y);
          c.args.insert(c.args.end(), {c1x, c1y, c2x, c2y, x, y});
          current = {x, y};
        }
        commands.push_back(std::move(c));
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
  const double radicand = den != 0 ? std::max(0.0, (rx2 * ry2 - rx2 * y1pp2 - ry2 * x1pp2) / den)
                                   : 0.0;
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
      while (pos < s.size() && (std::isspace(static_cast<unsigned char>(s[pos])) || s[pos] == ',')) {
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

void FillPolygon(RasterBuffer& buf, const std::vector<Point>& poly, const Color& color)
{
  if (poly.size() < 3) {
    return;
  }
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
        const double covered = std::min(1.0, std::min(static_cast<double>(x) + 1.0, x1) -
                                                 std::max(static_cast<double>(x), x0));
        buf.Blend(static_cast<std::size_t>(y) * static_cast<std::size_t>(buf.width) +
                      static_cast<std::size_t>(x),
                  color,
                  covered);
      }
    }
  }
}

void StrokeSegment(RasterBuffer& buf, Point p0, Point p1, double half_width, const Color& color)
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
                color);
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
              color);
  auto cap = [&](Point c) {
    const int steps = 16;
    std::vector<Point> circle;
    circle.reserve(static_cast<std::size_t>(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
      const double a = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(steps);
      circle.push_back({c.x + half_width * std::cos(a), c.y + half_width * std::sin(a)});
    }
    FillPolygon(buf, circle, color);
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
                 const Color& fill_color,
                 const Color& stroke_color)
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
    FillPolygon(buf, device, fill_color);
  }
  if (stroke && stroke_width > 0) {
    // stroke-width is in user units; the points were already transformed into
    // device space, so scale the width by the transform's average scale.
    const double scale =
        (std::sqrt(transform.a * transform.a + transform.b * transform.b) +
         std::sqrt(transform.c * transform.c + transform.d * transform.d)) /
        2.0;
    const double half = stroke_width / 2.0 * scale;
    for (std::size_t i = 0; i + 1 < device.size(); ++i) {
      StrokeSegment(buf, device[i], device[i + 1], half, stroke_color);
    }
    if (closed && device.size() >= 3) {
      StrokeSegment(buf, device.back(), device.front(), half, stroke_color);
    }
  }
}

void RenderPath(RasterBuffer& buf,
                const Mat& transform,
                std::string_view d,
                bool fill,
                bool stroke,
                double stroke_width,
                const Color& fill_color,
                const Color& stroke_color)
{
  const std::vector<PathCmd> commands = ParsePathData(d);
  Point current{0, 0};
  Point start{0, 0};
  std::vector<Point> poly;

  auto flush = [&]() {
    if (poly.size() >= 2) {
      RenderShape(buf, transform, poly, /*closed=*/false, fill, stroke, stroke_width, fill_color,
                  stroke_color);
    }
    poly.clear();
  };

  for (const PathCmd& cmd : commands) {
    const std::vector<double>& a = cmd.args;
    switch (cmd.cmd) {
      case 'M':
        flush();
        current = {a[0], a[1]};
        start = current;
        poly = {current};
        break;
      case 'L':
        for (std::size_t k = 0; k + 1 < a.size(); k += 2) {
          current = {a[k], a[k + 1]};
          poly.push_back(current);
        }
        break;
      case 'H':
        current.x = a[0];
        poly.push_back(current);
        break;
      case 'V':
        current.y = a[0];
        poly.push_back(current);
        break;
      case 'C':
        for (std::size_t k = 0; k + 5 < a.size(); k += 6) {
          const Point p0 = current;
          const Point p1{a[k], a[k + 1]};
          const Point p2{a[k + 2], a[k + 3]};
          const Point p3{a[k + 4], a[k + 5]};
          std::vector<Point> flat;
          FlattenCubic(flat, p0, p1, p2, p3, 8);
          poly.insert(poly.end(), flat.begin(), flat.end());
          current = p3;
        }
        break;
      case 'S':
        for (std::size_t k = 0; k + 3 < a.size(); k += 4) {
          const Point p0 = current;
          const Point p1 = poly.size() >= 2 ? poly[poly.size() - 2] : current;
          const Point p2{a[k], a[k + 1]};
          const Point p3{a[k + 2], a[k + 3]};
          std::vector<Point> flat;
          FlattenCubic(flat, p0, p1, p2, p3, 8);
          poly.insert(poly.end(), flat.begin(), flat.end());
          current = p3;
        }
        break;
      case 'Q':
        for (std::size_t k = 0; k + 3 < a.size(); k += 4) {
          const Point p0 = current;
          const Point p1{a[k], a[k + 1]};
          const Point p2{a[k + 2], a[k + 3]};
          std::vector<Point> flat;
          FlattenQuadratic(flat, p0, p1, p2, 8);
          poly.insert(poly.end(), flat.begin(), flat.end());
          current = p2;
        }
        break;
      case 'T':
        for (std::size_t k = 0; k + 1 < a.size(); k += 2) {
          const Point p0 = current;
          const Point p1 = poly.size() >= 2 ? poly[poly.size() - 2] : current;
          const Point p2{a[k], a[k + 1]};
          std::vector<Point> flat;
          FlattenQuadratic(flat, p0, p1, p2, 8);
          poly.insert(poly.end(), flat.begin(), flat.end());
          current = p2;
        }
        break;
      case 'A':
        for (std::size_t k = 0; k + 6 < a.size(); k += 7) {
          const Point p0 = current;
          const Point p1{a[k + 5], a[k + 6]};
          std::vector<Point> flat;
          FlattenArc(flat, p0, a[k], a[k + 1], a[k + 2], a[k + 3] != 0, a[k + 4] != 0, p1);
          poly.insert(poly.end(), flat.begin(), flat.end());
          current = p1;
        }
        break;
      case 'Z':
        if (poly.size() >= 2) {
          RenderShape(buf, transform, poly, /*closed=*/true, fill, stroke, stroke_width,
                      fill_color, stroke_color);
        }
        poly.clear();
        current = start;
        break;
      default:
        break;
    }
  }
  flush();
}

// ---------------------------------------------------------------------------
// Tree rendering
// ---------------------------------------------------------------------------

struct PaintState
{
  bool has_fill = true;
  Color fill{0, 0, 0, 1};
  bool has_stroke = false;
  Color stroke{0, 0, 0, 1};
  double stroke_width = 1;
};

void ApplyPaintAttr(PaintState& state, std::string_view attr, std::string_view value)
{
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
    state.fill.a = std::max(0.0, std::min(1.0, std::strtod(std::string(value).c_str(), nullptr)));
  } else if (attr == "stroke-opacity") {
    state.stroke.a =
        std::max(0.0, std::min(1.0, std::strtod(std::string(value).c_str(), nullptr)));
  } else if (attr == "opacity") {
    const double o = std::max(0.0, std::min(1.0, std::strtod(std::string(value).c_str(), nullptr)));
    state.fill.a *= o;
    state.stroke.a *= o;
  }
}

double AttrNumber(const Element& element, std::string_view key, double dflt)
{
  const std::string* v = element.Attr(key);
  return v == nullptr || v->empty() ? dflt : std::strtod(v->c_str(), nullptr);
}

void RenderElement(RasterBuffer& buf,
                   const Element& element,
                   const Mat& parent,
                   PaintState state)
{
  for (const auto& attr : element.attrs) {
    if (attr.first == "fill" || attr.first == "stroke" || attr.first == "stroke-width" ||
        attr.first == "fill-opacity" || attr.first == "stroke-opacity" || attr.first == "opacity") {
      ApplyPaintAttr(state, attr.first, attr.second);
    }
  }
  Mat local = parent;
  if (const std::string* t = element.Attr("transform")) {
    Mat m;
    if (ParseTransform(*t, m)) {
      local = parent * m;
    }
  }

  if (element.name == "svg" || element.name == "g" || element.name == "a") {
    for (const Element& child : element.children) {
      RenderElement(buf, child, local, state);
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
      RenderShape(buf, local, smooth, true, state.has_fill, state.has_stroke, state.stroke_width,
                  state.fill, state.stroke);
      return;
    }
    RenderShape(buf, local, {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}, true,
                state.has_fill, state.has_stroke, state.stroke_width, state.fill, state.stroke);
    return;
  }
  if (element.name == "circle" || element.name == "ellipse") {
    const double cx = AttrNumber(element, "cx", 0);
    const double cy = AttrNumber(element, "cy", 0);
    const double rx = element.name == "ellipse" ? AttrNumber(element, "rx", 0)
                                                : AttrNumber(element, "r", 0);
    const double ry = element.name == "ellipse" ? AttrNumber(element, "ry", 0) : rx;
    std::vector<Point> pts;
    const int steps = 64;
    pts.reserve(static_cast<std::size_t>(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
      const double a = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(steps);
      pts.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
    }
    RenderShape(buf, local, pts, true, state.has_fill, state.has_stroke, state.stroke_width,
                state.fill, state.stroke);
    return;
  }
  if (element.name == "line") {
    RenderShape(buf, local,
                {{AttrNumber(element, "x1", 0), AttrNumber(element, "y1", 0)},
                 {AttrNumber(element, "x2", 0), AttrNumber(element, "y2", 0)}},
                false, false, state.has_stroke, state.stroke_width, state.fill, state.stroke);
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
    RenderShape(buf, local, pts, element.name == "polygon", state.has_fill, state.has_stroke,
                state.stroke_width, state.fill, state.stroke);
    return;
  }
  if (element.name == "path") {
    if (const std::string* d = element.Attr("d")) {
      RenderPath(buf, local, *d, state.has_fill, state.has_stroke, state.stroke_width, state.fill,
                 state.stroke);
    }
    return;
  }
  if (element.name == "defs" || element.name == "symbol" || element.name == "marker" ||
      element.name == "clippath" || element.name == "mask" || element.name == "pattern") {
    return; // skipped content
  }
  for (const Element& child : element.children) {
    RenderElement(buf, child, local, state);
  }
}

} // namespace

bool IsSvg(std::string_view data)
{
  const std::size_t pos = SkipXmlProlog(data, 0);
  return pos + 4 <= data.size() && data.substr(pos, 4) == "<svg";
}

base::Result<Image> DecodeSvg(std::string_view data)
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

  if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
    return base::Err(base::Error::Parse("invalid SVG dimensions"));
  }
  const int out_w = static_cast<int>(std::ceil(width));
  const int out_h = static_cast<int>(std::ceil(height));
  RasterBuffer buf;
  buf.Init(out_w * 2, out_h * 2);

  Mat to_device = Mat::Scale(2.0, 2.0);
  if (has_viewbox && vbw > 0 && vbh > 0) {
    const double scale = std::min(static_cast<double>(buf.width) / vbw,
                                  static_cast<double>(buf.height) / vbh);
    const double tx = (static_cast<double>(buf.width) - vbw * scale) / 2.0 - vbx * scale;
    const double ty = (static_cast<double>(buf.height) - vbh * scale) / 2.0 - vby * scale;
    to_device = Mat::Translate(tx, ty) * Mat::Scale(scale, scale);
  } else if (has_viewbox) {
    to_device = Mat::Translate(-vbx * 2.0, -vby * 2.0) * Mat::Scale(2.0, 2.0);
  }

  PaintState state;
  RenderElement(buf, root, to_device, state);

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
          const std::size_t idx = static_cast<std::size_t>(y * 2 + dy) *
                                      static_cast<std::size_t>(buf.width) +
                                  static_cast<std::size_t>(x * 2 + dx);
          ar += buf.r[idx];
          ag += buf.g[idx];
          ab += buf.b[idx];
          aa += buf.a[idx];
        }
      }
      const double inv = aa > 0 ? 4.0 / aa : 0;
      const std::size_t idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(out_w) +
                               static_cast<std::size_t>(x)) * 4;
      const double alpha = std::min(1.0, aa / 4.0);
      image.rgba[idx + 0] = static_cast<uint8_t>(std::min(255.0, ar * inv) + 0.5);
      image.rgba[idx + 1] = static_cast<uint8_t>(std::min(255.0, ag * inv) + 0.5);
      image.rgba[idx + 2] = static_cast<uint8_t>(std::min(255.0, ab * inv) + 0.5);
      image.rgba[idx + 3] = static_cast<uint8_t>(alpha * 255.0 + 0.5);
    }
  }
  return base::Ok(std::move(image));
}

} // namespace neko::image
