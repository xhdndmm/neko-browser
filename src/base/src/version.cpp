#include "neko/base/version.h"

namespace neko::base {

std::string_view GetVersionString()
{
  return NEKO_VERSION_STRING;
}
std::string_view GetVersionTriple()
{
  return NEKO_VERSION_STRING;
}
std::string_view GetProjectName()
{
  return NEKO_PROJECT_NAME;
}

std::string_view GetUserAgent()
{
  // Conventional browser UA (see version.h).  Concatenated at compile time.
  return "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
         "Chrome/120.0.0.0 Safari/537.36 NekoBrowser/" NEKO_VERSION_STRING;
}

} // namespace neko::base
