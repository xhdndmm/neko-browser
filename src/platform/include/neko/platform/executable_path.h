#pragma once

// The path of this process's executable (AGENTS.md §11: the per-OS lookup
// lives in the platform layer).  Returns an empty string when the OS does
// not provide one.

#include <string>

namespace neko::platform {

std::string SelfExecutablePath();

} // namespace neko::platform
