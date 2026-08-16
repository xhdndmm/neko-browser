#include "neko/browser/renderer_host.h"

#include "neko/base/logging.h"
#include "neko/ipc/subprocess.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace neko::browser {

std::string SelfExecutablePath()
{
#ifdef _WIN32
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

RendererHost::RendererHost(std::string executable) : executable_(std::move(executable)) {}

base::Result<RendererLoadResult>
RendererHost::Load(const std::string& target, int width, int height)
{
  if (executable_.empty()) {
    return base::Err(base::Error::InvalidArgument("no renderer executable"));
  }
  std::vector<std::string> argv = {executable_, "--renderer-child", "--log-level", "warning"};
  auto spawned = ipc::Subprocess::Spawn(argv);
  if (!spawned.has_value()) {
    return base::Err(spawned.error());
  }
  ipc::Subprocess child = std::move(spawned.value());

  const RendererLoadRequest request{target, width, height};
  const auto encoded = EncodeLoadRequest(request);
  if (!encoded.has_value()) {
    child.Terminate();
    return base::Err(encoded.error());
  }
  if (!child.channel().Send(encoded.value())) {
    child.Terminate();
    return base::Err(base::Error::Io("failed to send the load request to the renderer"));
  }

  const auto reply = child.channel().Receive();
  const int exit_code = child.Wait();
  if (!reply.has_value()) {
    return base::Err(reply.error());
  }
  const auto decoded = DecodeLoadResult(reply.value());
  if (!decoded.has_value()) {
    return base::Err(decoded.error());
  }
  RendererLoadResult result = std::move(decoded.value());
  if (!result.ok) {
    if (!result.error.empty()) {
      return base::Err(base::Error::Io("renderer child failed: " + result.error));
    }
    return base::Err(base::Error::Io("renderer child failed with no error message"));
  }
  if (exit_code != 0) {
    NEKO_LOG_WARNING("renderer child exited with code " + std::to_string(exit_code) +
                     " after replying");
  }
  return result;
}

} // namespace neko::browser
