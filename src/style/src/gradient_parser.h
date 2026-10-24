#pragma once

// CSS background-image gradient value parsing (CSS Images 3 §3.1.1).
//
// Split out of style_engine.cpp: the cascade and computed-style code should
// not also carry the per-value grammar of every background function.  The
// functions here are style-internal (no other module consumes them).

#include "neko/style/computed_style.h"

#include <optional>
#include <string_view>
#include <vector>

namespace neko::style::internal {

// A resolved background-clip for one background layer.  |specified| is false
// when the declaration does not name a clip (callers keep the layer default);
// |clip| is nullopt for an unsupported keyword (`text`), which means the
// gradient layer cannot be painted and is dropped.
struct BackgroundLayerClip
{
  bool specified = false;
  std::optional<LinearGradient::Clip> clip;
};

// Parses one `linear-gradient(...)` layer; nullopt for any other function or
// a malformed declaration (the caller then leaves the layer alone).
std::optional<LinearGradient> ParseLinearGradientLayer(std::string_view layer);

// Splits a comma-separated CSS value at the top level: commas inside function
// arguments (`linear-gradient(...)`, `rgba(...)`) or quoted strings do not
// split.  Pieces keep their surrounding whitespace.
std::vector<std::string_view> SplitTopLevelCommas(std::string_view value);

// The background-clip keyword of one layer (defaults to |specified| = false).
BackgroundLayerClip ParseBackgroundLayerClip(std::string_view layer);

// Parses a `background-clip: <box>[, <box>]*` list, one entry per layer.
std::vector<std::optional<LinearGradient::Clip>> ParseBackgroundClipList(std::string_view value);

// Appends every linear-gradient layer of |value| to |out| (declaration order,
// first = top).  |clip_list| maps by layer index (cycled when shorter, per
// CSS Backgrounds 3 §2.1); without a list every layer defaults to border-box.
void AppendLinearGradientLayers(std::string_view value,
                                const std::vector<std::optional<LinearGradient::Clip>>& clip_list,
                                std::vector<LinearGradient>& out);

} // namespace neko::style::internal
