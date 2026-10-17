// SelfExecutablePath: one per-OS lookup, compiled on every platform (the
// #ifdefs are allowed here -- this file IS the platform layer, AGENTS.md §11).

#include "neko/platform/executable_path.h"

#include <cstdint>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace neko::platform {

std::string SelfExecutablePath()
{
#if defined(_WIN32)
  wchar_t buffer[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  if (length == 0) {
    return {};
  }
  const int size = WideCharToMultiByte(
      CP_UTF8, 0, buffer, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
  std::string path(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(
      CP_UTF8, 0, buffer, static_cast<int>(length), path.data(), size, nullptr, nullptr);
  return path;
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string path(size, '\0');
  if (_NSGetExecutablePath(path.data(), &size) != 0) {
    return {};
  }
  return path;
#else
  std::string path(4096, '\0');
  const ssize_t length = ::readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (length < 0) {
    return {};
  }
  path.resize(static_cast<std::size_t>(length));
  return path;
#endif
}

} // namespace neko::platform
