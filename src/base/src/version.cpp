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
  // Simplified browser UA: conventional "Mozilla/5.0 (<system info>)" prefix
  // (which web servers use to decide what content to serve) plus a
  // NekoBrowser token carrying the version.  See version.h.
#if defined(_WIN32)
  return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) NekoBrowser/" NEKO_VERSION_STRING;
#elif defined(__APPLE__)
  return "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) NekoBrowser/" NEKO_VERSION_STRING;
#else
  return "Mozilla/5.0 (X11; Linux x86_64) NekoBrowser/" NEKO_VERSION_STRING;
#endif
}

} // namespace neko::base
