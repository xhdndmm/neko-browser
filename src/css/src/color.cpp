#include "neko/css/color.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace neko::css {
namespace {

int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

bool ParseHexColor(std::string_view text, Color& out) {
  // text excludes the leading '#'.
  std::string hex;
  for (const char c : text) {
    if (HexValue(c) < 0) {
      return false;
    }
    hex.push_back(c);
  }
  auto channel = [&](size_t i, size_t len) -> int {
    int value = 0;
    for (size_t k = 0; k < len; ++k) {
      value = value * 16 + HexValue(hex[i + k]);
    }
    if (len == 1) {
      value = value * 16 + value;  // #abc -> #aabbcc
    }
    return value;
  };
  if (hex.size() == 3) {
    out.r = static_cast<uint8_t>(channel(0, 1));
    out.g = static_cast<uint8_t>(channel(1, 1));
    out.b = static_cast<uint8_t>(channel(2, 1));
    out.a = 255;
    return true;
  }
  if (hex.size() == 4) {
    out.r = static_cast<uint8_t>(channel(0, 1));
    out.g = static_cast<uint8_t>(channel(1, 1));
    out.b = static_cast<uint8_t>(channel(2, 1));
    out.a = static_cast<uint8_t>(channel(3, 1));
    return true;
  }
  if (hex.size() == 6) {
    out.r = static_cast<uint8_t>(channel(0, 2));
    out.g = static_cast<uint8_t>(channel(2, 2));
    out.b = static_cast<uint8_t>(channel(4, 2));
    out.a = 255;
    return true;
  }
  if (hex.size() == 8) {
    out.r = static_cast<uint8_t>(channel(0, 2));
    out.g = static_cast<uint8_t>(channel(2, 2));
    out.b = static_cast<uint8_t>(channel(4, 2));
    out.a = static_cast<uint8_t>(channel(6, 2));
    return true;
  }
  return false;
}

// Parses a number or percentage component inside rgb()/rgba().
bool ParseChannel(std::string_view text, float& out, bool& is_percent) {
  std::string_view t = text;
  if (!t.empty() && t.back() == '%') {
    is_percent = true;
    t.remove_suffix(1);
  }
  // strtof-based parse.
  std::string value(t);
  char* end = nullptr;
  const float parsed = std::strtof(value.c_str(), &end);
  if (end == value.c_str() || *end != '\0') {
    return false;
  }
  out = parsed;
  return true;
}

bool ParseRgbFunction(std::string_view text, Color& out) {
  // text like "255, 0, 128" or "100%, 0%, 50%" (alpha optional).
  std::string_view rest = text;
  float channels[4] = {0, 0, 0, 1};
  bool percent[3] = {false, false, false};
  int count = 0;
  while (count < 4 && !rest.empty()) {
    // skip whitespace and commas
    while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t' || rest[0] == ',')) {
      rest.remove_prefix(1);
    }
    if (rest.empty()) {
      break;
    }
    size_t end = 0;
    while (end < rest.size() && rest[end] != ',' && rest[end] != ' ' && rest[end] != '\t') {
      ++end;
    }
    const std::string_view part = rest.substr(0, end);
    float value = 0;
    bool is_percent = false;
    if (!ParseChannel(part, value, is_percent)) {
      return false;
    }
    if (count < 3) {
      channels[count] = value;
      percent[count] = is_percent;
    } else {
      channels[3] = value;  // alpha
    }
    ++count;
    rest.remove_prefix(end);
  }
  if (count < 3) {
    return false;
  }
  auto to_byte = [](float v, bool p) -> uint8_t {
    if (p) {
      v = v * 255.0f / 100.0f;
    }
    if (v < 0) {
      v = 0;
    }
    if (v > 255) {
      v = 255;
    }
    return static_cast<uint8_t>(v + 0.5f);
  };
  out.r = to_byte(channels[0], percent[0]);
  out.g = to_byte(channels[1], percent[1]);
  out.b = to_byte(channels[2], percent[2]);
  out.a = static_cast<uint8_t>(channels[3] * 255.0f + 0.5f);
  return true;
}

struct NamedColor {
  std::string_view name;
  Color color;
};

constexpr NamedColor kNamedColors[] = {
    {"aliceblue", {240, 248, 255, 255}},   {"antiquewhite", {250, 235, 215, 255}},
    {"aqua", {0, 255, 255, 255}},          {"aquamarine", {127, 255, 212, 255}},
    {"azure", {240, 255, 255, 255}},       {"beige", {245, 245, 220, 255}},
    {"bisque", {255, 228, 196, 255}},      {"black", {0, 0, 0, 255}},
    {"blue", {0, 0, 255, 255}},            {"blueviolet", {138, 43, 226, 255}},
    {"brown", {165, 42, 42, 255}},         {"burlywood", {222, 184, 135, 255}},
    {"cadetblue", {95, 158, 160, 255}},    {"chartreuse", {127, 255, 0, 255}},
    {"chocolate", {210, 105, 30, 255}},    {"coral", {255, 127, 80, 255}},
    {"cornflowerblue", {100, 149, 237, 255}}, {"crimson", {220, 20, 60, 255}},
    {"cyan", {0, 255, 255, 255}},          {"darkblue", {0, 0, 139, 255}},
    {"darkcyan", {0, 139, 139, 255}},      {"darkgoldenrod", {184, 134, 11, 255}},
    {"darkgray", {169, 169, 169, 255}},    {"darkgreen", {0, 100, 0, 255}},
    {"darkgrey", {169, 169, 169, 255}},    {"darkkhaki", {189, 183, 107, 255}},
    {"darkmagenta", {139, 0, 139, 255}},   {"darkolivegreen", {85, 107, 47, 255}},
    {"darkorange", {255, 140, 0, 255}},    {"darkorchid", {153, 50, 204, 255}},
    {"darkred", {139, 0, 0, 255}},         {"darksalmon", {233, 150, 122, 255}},
    {"darkseagreen", {143, 188, 143, 255}}, {"darkslateblue", {72, 61, 139, 255}},
    {"darkslategray", {47, 79, 79, 255}},  {"darkslategrey", {47, 79, 79, 255}},
    {"darkturquoise", {0, 206, 209, 255}}, {"darkviolet", {148, 0, 211, 255}},
    {"deeppink", {255, 20, 147, 255}},     {"deepskyblue", {0, 191, 255, 255}},
    {"dimgray", {105, 105, 105, 255}},     {"dimgrey", {105, 105, 105, 255}},
    {"dodgerblue", {30, 144, 255, 255}},   {"firebrick", {178, 34, 34, 255}},
    {"floralwhite", {255, 250, 240, 255}}, {"forestgreen", {34, 139, 34, 255}},
    {"fuchsia", {255, 0, 255, 255}},       {"gainsboro", {220, 220, 220, 255}},
    {"ghostwhite", {248, 248, 255, 255}},  {"gold", {255, 215, 0, 255}},
    {"goldenrod", {218, 165, 32, 255}},    {"gray", {128, 128, 128, 255}},
    {"green", {0, 128, 0, 255}},           {"greenyellow", {173, 255, 47, 255}},
    {"grey", {128, 128, 128, 255}},        {"honeydew", {240, 255, 240, 255}},
    {"hotpink", {255, 105, 180, 255}},     {"indianred", {205, 92, 92, 255}},
    {"indigo", {75, 0, 130, 255}},         {"ivory", {255, 255, 240, 255}},
    {"khaki", {240, 230, 140, 255}},       {"lavender", {230, 230, 250, 255}},
    {"lavenderblush", {255, 240, 245, 255}}, {"lawngreen", {124, 252, 0, 255}},
    {"lemonchiffon", {255, 250, 205, 255}}, {"lightblue", {173, 216, 230, 255}},
    {"lightcoral", {240, 128, 128, 255}},  {"lightcyan", {224, 255, 255, 255}},
    {"lightgoldenrodyellow", {250, 250, 210, 255}}, {"lightgray", {211, 211, 211, 255}},
    {"lightgreen", {144, 238, 144, 255}},  {"lightgrey", {211, 211, 211, 255}},
    {"lightpink", {255, 182, 193, 255}},   {"lightsalmon", {255, 160, 122, 255}},
    {"lightseagreen", {32, 178, 170, 255}}, {"lightskyblue", {135, 206, 250, 255}},
    {"lightslategray", {119, 136, 153, 255}}, {"lightslategrey", {119, 136, 153, 255}},
    {"lightsteelblue", {176, 196, 222, 255}}, {"lightyellow", {255, 255, 224, 255}},
    {"lime", {0, 255, 0, 255}},            {"limegreen", {50, 205, 50, 255}},
    {"linen", {250, 240, 230, 255}},       {"magenta", {255, 0, 255, 255}},
    {"maroon", {128, 0, 0, 255}},          {"mediumaquamarine", {102, 205, 170, 255}},
    {"mediumblue", {0, 0, 205, 255}},      {"mediumorchid", {186, 85, 211, 255}},
    {"mediumpurple", {147, 112, 219, 255}}, {"mediumseagreen", {60, 179, 113, 255}},
    {"mediumslateblue", {123, 104, 238, 255}}, {"mediumspringgreen", {0, 250, 154, 255}},
    {"mediumturquoise", {72, 209, 204, 255}}, {"mediumvioletred", {199, 21, 133, 255}},
    {"midnightblue", {25, 25, 112, 255}},  {"mintcream", {245, 255, 250, 255}},
    {"mistyrose", {255, 228, 225, 255}},   {"moccasin", {255, 228, 181, 255}},
    {"navajowhite", {255, 222, 173, 255}}, {"navy", {0, 0, 128, 255}},
    {"oldlace", {253, 245, 230, 255}},     {"olive", {128, 128, 0, 255}},
    {"olivedrab", {107, 142, 35, 255}},    {"orange", {255, 165, 0, 255}},
    {"orangered", {255, 69, 0, 255}},      {"orchid", {218, 112, 214, 255}},
    {"palegoldenrod", {238, 232, 170, 255}}, {"palegreen", {152, 251, 152, 255}},
    {"paleturquoise", {175, 238, 238, 255}}, {"palevioletred", {219, 112, 147, 255}},
    {"papayawhip", {255, 239, 213, 255}},  {"peachpuff", {255, 218, 185, 255}},
    {"peru", {205, 133, 63, 255}},         {"pink", {255, 192, 203, 255}},
    {"plum", {221, 160, 221, 255}},        {"powderblue", {176, 224, 230, 255}},
    {"purple", {128, 0, 128, 255}},        {"rebeccapurple", {102, 51, 153, 255}},
    {"red", {255, 0, 0, 255}},             {"rosybrown", {188, 143, 143, 255}},
    {"royalblue", {65, 105, 225, 255}},    {"saddlebrown", {139, 69, 19, 255}},
    {"salmon", {250, 128, 114, 255}},      {"sandybrown", {244, 164, 96, 255}},
    {"seagreen", {46, 139, 87, 255}},      {"seashell", {255, 245, 238, 255}},
    {"sienna", {160, 82, 45, 255}},        {"silver", {192, 192, 192, 255}},
    {"skyblue", {135, 206, 235, 255}},     {"slateblue", {106, 90, 205, 255}},
    {"slategray", {112, 128, 144, 255}},   {"slategrey", {112, 128, 144, 255}},
    {"snow", {255, 250, 250, 255}},        {"springgreen", {0, 255, 127, 255}},
    {"steelblue", {70, 130, 180, 255}},    {"tan", {210, 180, 140, 255}},
    {"teal", {0, 128, 128, 255}},          {"thistle", {216, 191, 216, 255}},
    {"tomato", {255, 99, 71, 255}},        {"transparent", {0, 0, 0, 0}},
    {"turquoise", {64, 224, 208, 255}},    {"violet", {238, 130, 238, 255}},
    {"wheat", {245, 222, 179, 255}},       {"white", {255, 255, 255, 255}},
    {"whitesmoke", {245, 245, 245, 255}},  {"yellow", {255, 255, 0, 255}},
    {"yellowgreen", {154, 205, 50, 255}},
};

}  // namespace

std::optional<Color> ParseColor(std::string_view text) {
  std::string_view t = text;
  while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) {
    t.remove_prefix(1);
  }
  while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) {
    t.remove_suffix(1);
  }
  if (t.empty()) {
    return std::nullopt;
  }
  if (t[0] == '#') {
    Color color;
    if (ParseHexColor(t.substr(1), color)) {
      return color;
    }
    return std::nullopt;
  }
  if (t.rfind("rgb", 0) == 0) {
    const size_t open = t.find('(');
    if (open != std::string_view::npos && t.back() == ')') {
      Color color;
      if (ParseRgbFunction(t.substr(open + 1, t.size() - open - 2), color)) {
        return color;
      }
    }
    return std::nullopt;
  }
  // Named colors (case-insensitive).
  for (const NamedColor& entry : kNamedColors) {
    if (entry.name.size() == t.size()) {
      bool equal = true;
      for (size_t i = 0; i < t.size(); ++i) {
        char a = entry.name[i];
        char b = t[i];
        if (a >= 'A' && a <= 'Z') {
          a = static_cast<char>(a + 32);
        }
        if (b >= 'A' && b <= 'Z') {
          b = static_cast<char>(b + 32);
        }
        if (a != b) {
          equal = false;
          break;
        }
      }
      if (equal) {
        return entry.color;
      }
    }
  }
  return std::nullopt;
}

}  // namespace neko::css
