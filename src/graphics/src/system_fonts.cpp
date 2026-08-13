#include "neko/graphics/system_fonts.h"

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
// fontconfig).  M3 extends this with directory scans and family matching.
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
      return {dir + "msyh.ttc", dir + "simhei.ttf"};
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
              "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
              "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"};
  }
#endif
  return {};
}

}  // namespace

std::optional<std::string> FindSystemFont(GenericFamily family) {
  for (const std::string& path : Candidates(family)) {
    if (FileExists(path)) {
      return path;
    }
  }
  return std::nullopt;
}

}  // namespace neko::graphics
