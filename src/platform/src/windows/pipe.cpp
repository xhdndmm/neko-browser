// Windows implementation of neko/platform/pipe.h.

#include "neko/platform/pipe.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace neko::platform {

PipeHandle InvalidPipeHandle()
{
  return INVALID_HANDLE_VALUE;
}

bool PipeHandleValid(PipeHandle handle)
{
  return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

PipePair StandardIoPipes()
{
  // The parent (platform::SpawnProcessWithStdio) redirected the child's
  // stdio to the pipe ends, so the standard handles are this end of the
  // channel.  The handles are used raw (ReadFile/WriteFile): no CRLF
  // translation or text-mode buffering can corrupt a binary frame.
  return PipePair{GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE)};
}

bool PipeReadExactly(PipeHandle handle, std::uint8_t* buf, std::size_t n, bool* clean_eof)
{
  *clean_eof = false;
  std::size_t done = 0;
  while (done < n) {
    DWORD got = 0;
    const BOOL ok = ReadFile(handle, buf + done, static_cast<DWORD>(n - done), &got, nullptr);
    if (!ok) {
      if (GetLastError() == ERROR_BROKEN_PIPE && done == 0) {
        *clean_eof = true;
      }
      return false;
    }
    if (got == 0) {
      if (done == 0) {
        *clean_eof = true;
      }
      return false;
    }
    done += got;
  }
  return true;
}

bool PipeWriteAll(PipeHandle handle, const std::uint8_t* buf, std::size_t n)
{
  std::size_t done = 0;
  while (done < n) {
    DWORD wrote = 0;
    const BOOL ok = WriteFile(handle, buf + done, static_cast<DWORD>(n - done), &wrote, nullptr);
    if (!ok) {
      return false;
    }
    if (wrote == 0) {
      return false;
    }
    done += wrote;
  }
  return true;
}

void ClosePipe(PipeHandle handle)
{
  if (PipeHandleValid(handle)) {
    CloseHandle(handle);
  }
}

} // namespace neko::platform
