#include "gradient_parser.h"

#include "neko/base/string_util.h"
#include "neko/css/color.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neko::style::internal {

bool StartsWithNoCase(std::string_view text, std::string_view prefix)
{
  return text.size() >= prefix.size() &&
         base::AsciiEqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

// Splits a comma-separated CSS list at the top level: commas inside function
// arguments (`linear-gradient(...)`, `rgba(...)`, `url(...)`) and quoted
// strings do not split.  Pieces are trimmed but keep their original case.
std::vector<std::string_view> SplitTopLevelCommas(std::string_view value)
{
  std::vector<std::string_view> parts;
  int depth = 0;
  char quote = '\0';
  std::size_t start = 0;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (quote != '\0') {
      if (c == quote) {
        quote = '\0';
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')' && depth > 0) {
      --depth;
    } else if (c == ',' && depth == 0) {
      parts.push_back(base::Trim(value.substr(start, i - start)));
      start = i + 1;
    }
  }
  parts.push_back(base::Trim(value.substr(start)));
  return parts;
}

// Splits on ASCII whitespace at parenthesis depth zero, so a color function
// such as `rgba(1, 2, 3, .5)` stays one token.
std::vector<std::string_view> SplitWhitespaceTopLevel(std::string_view value)
{
  std::vector<std::string_view> parts;
  int depth = 0;
  char quote = '\0';
  std::size_t start = std::string_view::npos;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (quote != '\0') {
      if (c == quote) {
        quote = '\0';
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      if (start == std::string_view::npos) {
        start = i;
      }
      continue;
    }
    if (c == '(') {
      ++depth;
    } else if (c == ')' && depth > 0) {
      --depth;
    }
    const bool space = depth == 0 && (c == ' ' || c == '\t' || c == '\n' || c == '\r');
    if (space) {
      if (start != std::string_view::npos) {
        parts.push_back(value.substr(start, i - start));
        start = std::string_view::npos;
      }
    } else if (start == std::string_view::npos) {
      start = i;
    }
  }
  if (start != std::string_view::npos) {
    parts.push_back(value.substr(start));
  }
  return parts;
}

std::optional<float> ParseFloatStrict(std::string_view text)
{
  if (text.empty()) {
    return std::nullopt;
  }
  const std::string owned(text);
  char* end = nullptr;
  const float value = std::strtof(owned.c_str(), &end);
  if (end == owned.c_str() || *end != '\0' || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<float> ParseAngleDegrees(std::string_view text)
{
  text = base::Trim(text);
  if (text.empty()) {
    return std::nullopt;
  }
  struct Unit
  {
    std::string_view suffix;
    float scale;
  };
  static constexpr Unit kUnits[] = {
      {"deg", 1.0F},
      {"grad", 0.9F},
      {"rad", 57.29577951308232F},
      {"turn", 360.0F},
  };
  for (const Unit& unit : kUnits) {
    if (text.size() > unit.suffix.size() &&
        base::ToLower(text.substr(text.size() - unit.suffix.size())) == unit.suffix) {
      const std::optional<float> number =
          ParseFloatStrict(text.substr(0, text.size() - unit.suffix.size()));
      if (!number.has_value()) {
        return std::nullopt;
      }
      return *number * unit.scale;
    }
  }
  // Unitless angles are only valid for zero (CSS Values 4 §6.1).
  if (const std::optional<float> number = ParseFloatStrict(text);
      number.has_value() && *number == 0.0F) {
    return 0.0F;
  }
  return std::nullopt;
}

// Parses `to <side> [<side>]` or an <angle>.  Corner keywords are mapped to
// the 45-degree diagonal of their quadrant: the exact CSS Images 3 angle
// depends on the box aspect ratio, which is not known while computing style.
bool ParseGradientDirection(std::string_view text, float& angle_deg)
{
  const std::vector<std::string_view> tokens = SplitWhitespaceTopLevel(base::Trim(text));
  if (tokens.empty()) {
    return false;
  }
  if (base::ToLower(tokens[0]) == "to") {
    bool top = false;
    bool bottom = false;
    bool left = false;
    bool right = false;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::string side = base::ToLower(tokens[i]);
      if (side == "top") {
        top = true;
      } else if (side == "bottom") {
        bottom = true;
      } else if (side == "left") {
        left = true;
      } else if (side == "right") {
        right = true;
      } else {
        return false;
      }
    }
    if ((top && bottom) || (left && right)) {
      return false;
    }
    if (top && right) {
      angle_deg = 45.0F;
    } else if (bottom && right) {
      angle_deg = 135.0F;
    } else if (bottom && left) {
      angle_deg = 225.0F;
    } else if (top && left) {
      angle_deg = 315.0F;
    } else if (right) {
      angle_deg = 90.0F;
    } else if (left) {
      angle_deg = 270.0F;
    } else if (bottom) {
      angle_deg = 180.0F;
    } else {
      angle_deg = 0.0F; // top
    }
    return true;
  }
  if (const std::optional<float> angle = ParseAngleDegrees(tokens[0]);
      angle.has_value() && tokens.size() == 1) {
    angle_deg = *angle;
    return true;
  }
  return false;
}

bool LooksLikeLength(std::string_view token)
{
  static constexpr std::string_view kUnits[] = {
      "px", "em", "rem", "ex", "ch", "vw", "vh", "vmin", "vmax", "pt", "pc", "cm", "mm", "in", "q"};
  const std::string lower = base::ToLower(token);
  for (const std::string_view unit : kUnits) {
    if (lower.size() > unit.size() && lower.substr(lower.size() - unit.size()) == unit) {
      return true;
    }
  }
  return lower == "0";
}

struct ParsedGradientStop
{
  css::Color color;
  std::optional<float> position; // resolved fraction, nullopt = auto
};

enum class StopParseResult
{
  kOk,
  kHint, // interpolation hint: not modelled, ignored
  kInvalid,
};

// Parses `<color> [<position>]{0,2}` (a two-position stop expands into two
// stops, CSS Images 4 §3.1).  Length positions are accepted syntactically but
// left unresolved (auto-distributed) -- only percentages are modelled.
StopParseResult ParseGradientStop(std::string_view text, std::vector<ParsedGradientStop>& out)
{
  const std::vector<std::string_view> tokens = SplitWhitespaceTopLevel(base::Trim(text));
  if (tokens.empty()) {
    return StopParseResult::kInvalid;
  }
  const std::optional<css::Color> color = css::ParseColor(tokens[0]);
  if (!color.has_value()) {
    // A bare percentage between stops is an interpolation hint; hints are not
    // modelled, so drop it rather than invalidating the gradient.
    bool all_percentages = true;
    for (const std::string_view token : tokens) {
      if (token.size() < 2 || token.back() != '%' ||
          !ParseFloatStrict(token.substr(0, token.size() - 1)).has_value()) {
        all_percentages = false;
        break;
      }
    }
    return all_percentages ? StopParseResult::kHint : StopParseResult::kInvalid;
  }

  std::vector<float> positions;
  for (std::size_t i = 1; i < tokens.size(); ++i) {
    const std::string_view token = tokens[i];
    if (token.size() >= 2 && token.back() == '%') {
      const std::optional<float> number = ParseFloatStrict(token.substr(0, token.size() - 1));
      if (!number.has_value()) {
        return StopParseResult::kInvalid;
      }
      positions.push_back(std::clamp(*number / 100.0F, 0.0F, 1.0F));
      if (positions.size() > 2) {
        return StopParseResult::kInvalid;
      }
    } else if (!LooksLikeLength(token)) {
      return StopParseResult::kInvalid;
    }
    // Length positions are ignored (auto-distributed); documented limitation.
  }

  ParsedGradientStop stop;
  stop.color = *color;
  if (positions.empty()) {
    out.push_back(stop);
  } else {
    for (const float position : positions) {
      stop.position = position;
      out.push_back(stop);
    }
  }
  return StopParseResult::kOk;
}

// CSS Images 3 §3.1.1 auto-distribution: a missing first/last stop sits at
// 0/1, and a run of missing positions is evenly spaced between its resolved
// neighbours.  Positions are then clamped to be non-decreasing.
std::vector<LinearGradientStop> ResolveGradientStops(std::vector<ParsedGradientStop> parsed)
{
  std::vector<LinearGradientStop> stops;
  if (parsed.size() < 2) {
    return stops;
  }
  if (!parsed.front().position.has_value()) {
    parsed.front().position = 0.0F;
  }
  if (!parsed.back().position.has_value()) {
    parsed.back().position = 1.0F;
  }
  std::size_t i = 1;
  while (i + 1 < parsed.size()) {
    if (parsed[i].position.has_value()) {
      ++i;
      continue;
    }
    std::size_t next = i;
    while (next < parsed.size() && !parsed[next].position.has_value()) {
      ++next;
    }
    if (next >= parsed.size()) {
      break;
    }
    const float start = *parsed[i - 1].position;
    const float end = *parsed[next].position;
    const float span = static_cast<float>(next - (i - 1));
    for (std::size_t k = i; k < next; ++k) {
      parsed[k].position = start + (end - start) * (static_cast<float>(k - (i - 1)) / span);
    }
    i = next;
  }
  float previous = 0.0F;
  for (const ParsedGradientStop& parsed_stop : parsed) {
    LinearGradientStop stop;
    stop.color = parsed_stop.color;
    stop.position = std::max(previous, parsed_stop.position.value_or(0.0F));
    previous = stop.position;
    stops.push_back(stop);
  }
  return stops;
}

// Parses one `linear-gradient(...)` layer.  Returns nullopt when the layer is
// not a linear gradient, is malformed, or uses a variant that is not modelled
// (repeating/radial/conic gradients, colour hints alone, fewer than two
// stops).  The caller decides the clip box.
std::optional<LinearGradient> ParseLinearGradientLayer(std::string_view layer)
{
  std::string_view text = base::Trim(layer);
  if (!StartsWithNoCase(text, "linear-gradient(")) {
    // repeating-linear-gradient / radial-gradient / conic-gradient are NOT
    // IMPLEMENTED; dropping the layer is better than painting it wrongly.
    return std::nullopt;
  }
  const std::size_t open = text.find('(');
  if (open == std::string_view::npos) {
    return std::nullopt;
  }
  int depth = 0;
  std::size_t close = std::string_view::npos;
  for (std::size_t i = open; i < text.size(); ++i) {
    if (text[i] == '(') {
      ++depth;
    } else if (text[i] == ')' && --depth == 0) {
      close = i;
      break;
    }
  }
  if (close == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view arguments = text.substr(open + 1, close - open - 1);
  const std::vector<std::string_view> pieces = SplitTopLevelCommas(arguments);

  LinearGradient gradient;
  std::size_t first_stop = 0;
  if (!pieces.empty() && ParseGradientDirection(pieces[0], gradient.angle_deg)) {
    first_stop = 1;
  }
  std::vector<ParsedGradientStop> parsed_stops;
  for (std::size_t i = first_stop; i < pieces.size(); ++i) {
    const StopParseResult result = ParseGradientStop(pieces[i], parsed_stops);
    if (result == StopParseResult::kInvalid) {
      return std::nullopt; // malformed declaration: ignore the whole layer
    }
  }
  gradient.stops = ResolveGradientStops(std::move(parsed_stops));
  if (gradient.stops.size() < 2) {
    return std::nullopt;
  }
  return gradient;
}

// Keyword tokens of one layer with every function(...) argument removed.
std::vector<std::string> BackgroundLayerKeywords(std::string_view layer)
{
  std::string stripped;
  stripped.reserve(layer.size());
  for (std::size_t i = 0; i < layer.size();) {
    const bool identifier_start =
        std::isalpha(static_cast<unsigned char>(layer[i])) != 0 || layer[i] == '-';
    if (identifier_start) {
      std::size_t end = i;
      while (end < layer.size()) {
        const unsigned char c = static_cast<unsigned char>(layer[end]);
        if (std::isalnum(c) == 0 && c != '-' && c != '_') {
          break;
        }
        ++end;
      }
      if (end < layer.size() && layer[end] == '(') {
        int depth = 0;
        std::size_t close = end;
        for (; close < layer.size(); ++close) {
          if (layer[close] == '(') {
            ++depth;
          } else if (layer[close] == ')' && --depth == 0) {
            ++close;
            break;
          }
        }
        stripped.push_back(' ');
        i = close;
        continue;
      }
      stripped.append(layer.substr(i, end - i));
      i = end;
      continue;
    }
    stripped.push_back(layer[i]);
    ++i;
  }
  std::vector<std::string> tokens;
  for (const std::string_view token : SplitWhitespaceTopLevel(stripped)) {
    tokens.push_back(base::ToLower(token));
  }
  return tokens;
}

std::optional<LinearGradient::Clip> ParseClipKeyword(std::string_view token)
{
  if (token == "border-box") {
    return LinearGradient::Clip::kBorderBox;
  }
  if (token == "padding-box") {
    return LinearGradient::Clip::kPaddingBox;
  }
  if (token == "content-box") {
    return LinearGradient::Clip::kContentBox;
  }
  return std::nullopt;
}

BackgroundLayerClip ParseBackgroundLayerClip(std::string_view layer)
{
  for (const std::string& token : BackgroundLayerKeywords(layer)) {
    if (const std::optional<LinearGradient::Clip> clip = ParseClipKeyword(token);
        clip.has_value()) {
      BackgroundLayerClip result;
      result.specified = true;
      result.clip = clip;
      return result;
    }
    // background-clip: text cannot be painted for a gradient layer; mark it
    // unsupported so the layer is dropped rather than painted as a rectangle.
    if (token == "text") {
      BackgroundLayerClip result;
      result.specified = true;
      return result;
    }
  }
  return BackgroundLayerClip{};
}

// Comma-separated background-clip list.  An entry that cannot be modelled
// (text, unknown keyword) becomes nullopt so the matching layer is dropped.
std::vector<std::optional<LinearGradient::Clip>> ParseBackgroundClipList(std::string_view value)
{
  std::vector<std::optional<LinearGradient::Clip>> clips;
  for (const std::string_view piece : SplitTopLevelCommas(value)) {
    const std::string token = base::ToLower(base::Trim(piece));
    if (const std::optional<LinearGradient::Clip> clip = ParseClipKeyword(token);
        clip.has_value()) {
      clips.push_back(clip);
    } else {
      clips.push_back(std::nullopt);
    }
  }
  return clips;
}

// Appends every linear-gradient layer of |value| to |out| (declaration order,
// first = top).  |clip_list| maps by layer index (cycled when shorter, per
// CSS Backgrounds 3 §2.1); without a list every layer defaults to border-box.
void AppendLinearGradientLayers(std::string_view value,
                                const std::vector<std::optional<LinearGradient::Clip>>& clip_list,
                                std::vector<LinearGradient>& out)
{
  std::size_t layer_index = 0;
  for (const std::string_view layer : SplitTopLevelCommas(value)) {
    if (std::optional<LinearGradient> gradient = ParseLinearGradientLayer(layer)) {
      bool supported = true;
      if (!clip_list.empty()) {
        const std::optional<LinearGradient::Clip>& clip = clip_list[layer_index % clip_list.size()];
        if (clip.has_value()) {
          gradient->clip = *clip;
        } else {
          supported = false; // background-clip: text / unknown
        }
      }
      if (supported) {
        out.push_back(std::move(*gradient));
      }
    }
    ++layer_index;
  }
}
} // namespace neko::style::internal
