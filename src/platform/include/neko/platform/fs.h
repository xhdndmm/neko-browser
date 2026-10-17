#pragma once

// OS filesystem primitives (AGENTS.md §11: Win32/POSIX calls live in the
// platform layer, never in engine code).  Implementations:
// src/platform/src/posix/fs.cpp and src/platform/src/windows/fs.cpp.

#include "neko/base/status.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace neko::platform {

// Creates one directory level.  Succeeds when it already exists.
base::Result<void> MakeOneDirectory(const std::string& path);

// Durably replaces |path| with |content|: the bytes hit stable storage and
// the replace is atomic (readers see the old or the new file, never a
// partial one).
base::Result<void> ReplaceFileAtomic(const std::string& path, std::string_view content);

// Creates |path| exclusively (fails if it exists) and returns a binary
// stdio stream over it.  Returns nullptr on failure.  The caller owns the
// stream.  Used for "write beside, then rename over" patterns (PpmWriter).
std::FILE* OpenExclusiveBinary(const std::string& path);

// Renames |from| over |to|, replacing an existing destination atomically
// where the OS supports it (POSIX rename; MoveFileEx on Windows).
bool ReplaceFileOverwrite(const std::string& from, const std::string& to);

// The OS process id, for unique temporary names.
unsigned long ProcessId();

} // namespace neko::platform
