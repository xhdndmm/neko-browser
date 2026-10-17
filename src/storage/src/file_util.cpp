#include "neko/storage/file_util.h"

#include "neko/platform/fs.h"

#include <cstdio>
#include <string>

namespace neko::storage {
namespace {

// Recursively creates |dir|.  Empty string or "." succeeds trivially.  The
// one-level creation is the OS seam (neko/platform/fs.h).
base::Result<void> MkdirAll(const std::string& dir)
{
  if (dir.empty() || dir == "." || dir == "/")
    return base::Error();
  // Create the parent first.
  const size_t slash = dir.find_last_of("/\\");
  if (slash != std::string::npos && slash > 0) {
    const std::string parent = dir.substr(0, slash);
    auto r = MkdirAll(parent);
    if (!r)
      return r;
  }
  return platform::MakeOneDirectory(dir);
}

} // namespace

base::Result<std::string> ReadFile(std::string_view path)
{
  FILE* f = std::fopen(std::string(path).c_str(), "rb");
  if (f == nullptr) {
    return base::Error::Io("cannot open '" + std::string(path) + "' for reading");
  }
  std::string data;
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    data.append(buf, n);
  }
  const bool failed = std::ferror(f) != 0;
  std::fclose(f);
  if (failed) {
    return base::Error::Io("read failed on '" + std::string(path) + "'");
  }
  return data;
}

base::Result<void> CreateDirectory(std::string_view dir)
{
  return MkdirAll(std::string(dir));
}

base::Result<void> WriteFileAtomic(std::string_view path, std::string_view content)
{
  const std::string path_str(path);
  const size_t slash = path_str.find_last_of("/\\");
  if (slash != std::string::npos && slash > 0) {
    auto r = MkdirAll(path_str.substr(0, slash));
    if (!r)
      return r;
  }
  return platform::ReplaceFileAtomic(path_str, content);
}

} // namespace neko::storage
