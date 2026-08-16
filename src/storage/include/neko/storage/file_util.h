#pragma once

#include "neko/base/status.h"

#include <string>
#include <string_view>

namespace neko::storage {

// Reads the entire file at |path| as bytes.
base::Result<std::string> ReadFile(std::string_view path);

// Creates |dir| and any missing parents (like `mkdir -p`).
base::Result<void> CreateDirectory(std::string_view dir);

// Atomically writes |content| to |path|: writes a sibling temporary file and
// renames it over the target, so a crash mid-write never leaves a truncated
// store behind.  The parent directory is created if needed.
base::Result<void> WriteFileAtomic(std::string_view path, std::string_view content);

} // namespace neko::storage
