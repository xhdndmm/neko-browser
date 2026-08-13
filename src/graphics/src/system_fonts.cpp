#include "neko/graphics/system_fonts.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace neko::graphics {
namespace {

bool FileExists(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  std::fclose(file);
  return true;
}

// Candidate full paths per generic family, tried in order.  Covers the common
// Linux distro layouts plus Windows/macOS defaults; intentionally small (no
// fontconfig).
std::vector<std::string> Candidates(GenericFamily family) {
#if defined(_WIN32)
  const std::string dir = "C:/Windows/Fonts/";
  switch (family) {
    case GenericFamily::kSansSerif:
      return {dir + "segoeui.ttf", dir + "arial.ttf"};
    case GenericFamily::kSerif:
      return {dir + "times.ttf", dir + "georgia.ttf"};
    case GenericFamily::kMonospace:
      return {dir + "consola.ttf", dir + "cour.ttf"};
    case GenericFamily::kCjkSans:
      return {dir + "msyh.ttc", dir + "simhei.ttf", dir + "simsun.ttc"};
  }
#elif defined(__APPLE__)
  const std::string sys = "/System/Library/Fonts/";
  const std::string sup = "/System/Library/Fonts/Supplemental/";
  switch (family) {
    case GenericFamily::kSansSerif:
      return {sys + "Helvetica.ttc", sup + "Arial.ttf"};
    case GenericFamily::kSerif:
      return {sys + "Times.ttc", sup + "Times New Roman.ttf"};
    case GenericFamily::kMonospace:
      return {sys + "Menlo.ttc", sup + "Courier New.ttf"};
    case GenericFamily::kCjkSans:
      return {sys + "PingFang.ttc", sup + "Songti.ttc"};
  }
#else
  switch (family) {
    case GenericFamily::kSansSerif:
      return {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
              "/usr/share/fonts/TTF/DejaVuSans.ttf",
              "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
              "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
              "/usr/share/fonts/dejavu/DejaVuSans.ttf"};
    case GenericFamily::kSerif:
      return {"/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
              "/usr/share/fonts/liberation/LiberationSerif-Regular.ttf",
              "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf"};
    case GenericFamily::kMonospace:
      return {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
              "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
              "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"};
    case GenericFamily::kCjkSans:
      return {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
              "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
              "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttf",
              "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
              "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"};
  }
#endif
  return {};
}

// Lowercased alphanumeric-only form of |s|, for forgiving filename matching.
std::string Fold(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  }
  return out;
}

// True when the folded filename contains the folded needle.
bool NameMatches(std::string_view filename, std::string_view needle) {
  const std::string f = Fold(filename);
  const std::string n = Fold(needle);
  return f.find(n) != std::string::npos;
}

}  // namespace

std::vector<std::string> FindSystemFonts(GenericFamily family) {
  std::vector<std::string> found;
  for (const std::string& path : Candidates(family)) {
    if (FileExists(path)) {
      found.push_back(path);
    }
  }
  return found;
}

std::string ResolveFamilyName(std::string_view name) {
  if (NameMatches(name, "noto sans cjk") || NameMatches(name, "source han sans") ||
      NameMatches(name, "wqy") || NameMatches(name, "wenquanyi")) {
    for (const std::string& path : FindSystemFonts(GenericFamily::kCjkSans)) {
      if (NameMatches(path, "notosanscjk") || NameMatches(path, "sourcehan") ||
          NameMatches(path, "wqy")) {
        return path;
      }
    }
    const std::vector<std::string> fonts = FindSystemFonts(GenericFamily::kCjkSans);
    if (!fonts.empty()) {
      return fonts[0];
    }
  }
  if (NameMatches(name, "microsoft yahei") || name == "微软雅黑") {
#if defined(_WIN32)
    return "C:/Windows/Fonts/msyh.ttc";
#endif
  }
  if (NameMatches(name, "pingfang")) {
#if defined(__APPLE__)
    return "/System/Library/Fonts/PingFang.ttc";
#endif
  }
  // Generic Latin families by filename match against the platform candidates.
  for (const GenericFamily family :
       {GenericFamily::kSansSerif, GenericFamily::kSerif, GenericFamily::kMonospace}) {
    for (const std::string& path : FindSystemFonts(family)) {
      if (NameMatches(path, name)) {
        return path;
      }
    }
  }
  return {};
}

std::string FindFontVariant(const std::string& base_path, int weight, bool italic) {
  if (weight < 600 && !italic) {
    return base_path;  // regular requested
  }
  const std::size_t slash = base_path.find_last_of("/\\");
  const std::size_t dot = base_path.find_last_of('.');
  if (slash == std::string::npos || dot == std::string::npos || dot <= slash + 1) {
    return base_path;
  }
  const std::string dir = base_path.substr(0, slash + 1);
  const std::string ext = base_path.substr(dot);
  std::string name = base_path.substr(slash + 1, dot - slash - 1);

  // Drop a trailing style marker so "LiberationSans-Regular.ttf" becomes
  // "LiberationSans" and "DejaVuSans.ttf" stays as-is.  DejaVu names its
  // italic faces "Oblique".
  for (const char* marker :
       {"-BoldItalic", "-BoldOblique", "-Italic", "-Oblique", "-Bold", "-Regular", "-Medium"}) {
    const std::string m = marker;
    if (name.size() > m.size() &&
        name.compare(name.size() - m.size(), m.size(), m) == 0) {
      name = name.substr(0, name.size() - m.size());
      break;
    }
  }

  // Preferred variants in order of specificity (both "Italic" and DejaVu's
  // "Oblique" naming are tried).
  std::vector<std::string> candidates;
  const bool bold = weight >= 600;
  if (bold && italic) {
    candidates = {name + "-BoldItalic", name + "BoldItalic", name + "-BoldOblique",
                  name + "BoldOblique"};
  } else if (bold) {
    candidates = {name + "-Bold", name + "Bold"};
  } else {
    candidates = {name + "-Italic", name + "Italic", name + "-Oblique", name + "Oblique"};
  }
  for (const std::string& candidate : candidates) {
    const std::string path = dir + candidate + ext;
    if (FileExists(path)) {
      return path;
    }
  }
  return base_path;
}

}  // namespace neko::graphics
