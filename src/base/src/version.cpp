#include "neko/base/version.h"

namespace neko::base {

std::string_view GetVersionString() { return NEKO_VERSION_STRING; }
std::string_view GetVersionTriple() { return NEKO_VERSION_STRING; }
std::string_view GetProjectName() { return NEKO_PROJECT_NAME; }

}  // namespace neko::base
