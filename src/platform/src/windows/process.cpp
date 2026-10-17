// Windows implementation of neko/platform/process.h.

#include "neko/platform/process.h"

#define WIN32_LEAN_AND_MEAN
#include <string>
#include <windows.h>

namespace neko::platform {
namespace {

// Quotes one argument for the CreateProcess command line: wraps it in quotes
// when it contains spaces/tabs and doubles inner quotes (backslash handling
// before quotes follows the standard CommandLineToArgvW rules).
std::wstring QuoteWindowsArg(const std::wstring& arg)
{
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
    return arg;
  }
  std::wstring out = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
    } else {
      out.append(backslashes, L'\\');
      out.push_back(c);
    }
    backslashes = 0;
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

std::wstring Utf8ToWide(const std::string& utf8)
{
  if (utf8.empty()) {
    return L"";
  }
  const int size =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
  return wide;
}

// Returns an inheritable duplicate of |h|, or nullptr when |h| is not a
// usable handle or duplication fails.  The caller closes a non-null result
// after CreateProcessW so the handle is not inherited by later subprocesses.
HANDLE DuplicateInheritable(HANDLE h)
{
  if (h == nullptr || h == INVALID_HANDLE_VALUE) {
    return nullptr;
  }
  HANDLE duplicate = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(),
                       h,
                       GetCurrentProcess(),
                       &duplicate,
                       0,
                       /*bInheritHandle=*/TRUE,
                       DUPLICATE_SAME_ACCESS)) {
    return nullptr;
  }
  return duplicate;
}

} // namespace

base::Result<ChildProcess> SpawnProcessWithStdio(const std::vector<std::string>& argv)
{
  if (argv.empty()) {
    return base::Err(base::Error::InvalidArgument("empty argv"));
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE child_stdin_read = nullptr;
  HANDLE child_stdin_write = nullptr;
  if (!CreatePipe(&child_stdin_read, &child_stdin_write, &sa, 0)) {
    return base::Err(base::Error::Io("CreatePipe(stdin) failed"));
  }
  if (!SetHandleInformation(child_stdin_read, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
    CloseHandle(child_stdin_read);
    CloseHandle(child_stdin_write);
    return base::Err(base::Error::Io("SetHandleInformation(stdin) failed"));
  }
  // The parent's end must not be inherited by future subprocesses.
  SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0);

  HANDLE child_stdout_read = nullptr;
  HANDLE child_stdout_write = nullptr;
  if (!CreatePipe(&child_stdout_read, &child_stdout_write, &sa, 0)) {
    CloseHandle(child_stdin_read);
    CloseHandle(child_stdin_write);
    return base::Err(base::Error::Io("CreatePipe(stdout) failed"));
  }
  if (!SetHandleInformation(child_stdout_write, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
    CloseHandle(child_stdin_read);
    CloseHandle(child_stdin_write);
    CloseHandle(child_stdout_read);
    CloseHandle(child_stdout_write);
    return base::Err(base::Error::Io("SetHandleInformation(stdout) failed"));
  }
  // The parent's end must not be inherited by future subprocesses.
  SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0);

  // Build the full command line (exe + arguments, all quoted).
  std::wstring command_line = QuoteWindowsArg(Utf8ToWide(argv[0]));
  for (std::size_t i = 1; i < argv.size(); ++i) {
    command_line.push_back(L' ');
    command_line += QuoteWindowsArg(Utf8ToWide(argv[i]));
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = child_stdin_read;
  startup.hStdOutput = child_stdout_write;
  // The child's diagnostics (NEKO_LOG_*, script console output) must not share
  // the protocol pipe: a single log line would be read as the next frame
  // header and rejected as "frame length exceeds the cap".  POSIX children
  // inherit the parent's stderr; mirror that with an inheritable duplicate of
  // the parent's standard error handle.  If the duplicate is unavailable the
  // child keeps the inherited handle value, whose writes simply fail — still
  // preferable to corrupting the channel.
  const HANDLE parent_stderr = GetStdHandle(STD_ERROR_HANDLE);
  HANDLE child_stderr = DuplicateInheritable(parent_stderr);
  startup.hStdError = child_stderr != nullptr ? child_stderr : parent_stderr;

  PROCESS_INFORMATION process{};
  const std::wstring exe = Utf8ToWide(argv[0]);
  const BOOL ok = CreateProcessW(exe.c_str(),
                                 command_line.data(),
                                 nullptr,
                                 nullptr,
                                 /*bInheritHandles=*/TRUE,
                                 0,
                                 nullptr,
                                 nullptr,
                                 &startup,
                                 &process);
  // The child now owns its copies; close the parent's.
  CloseHandle(child_stdin_read);
  CloseHandle(child_stdout_write);
  if (child_stderr != nullptr) {
    CloseHandle(child_stderr);
  }
  if (!ok) {
    CloseHandle(child_stdin_write);
    CloseHandle(child_stdout_read);
    return base::Err(base::Error::Io("CreateProcessW failed"));
  }
  CloseHandle(process.hThread);

  ChildProcess spawned;
  spawned.handle = process.hProcess;
  spawned.from_child_read = child_stdout_read;
  spawned.to_child_write = child_stdin_write;
  return spawned;
}

long ChildProcessId(const ChildProcess& child)
{
  if (child.handle == nullptr) {
    return 0;
  }
  return static_cast<long>(GetProcessId(static_cast<HANDLE>(child.handle)));
}

int WaitForChild(ChildProcess& child)
{
  if (child.handle == nullptr) {
    return -1;
  }
  WaitForSingleObject(static_cast<HANDLE>(child.handle), INFINITE);
  DWORD exit_code = 0;
  if (!GetExitCodeProcess(static_cast<HANDLE>(child.handle), &exit_code)) {
    CloseHandle(static_cast<HANDLE>(child.handle));
    child.handle = nullptr;
    return -1;
  }
  CloseHandle(static_cast<HANDLE>(child.handle));
  child.handle = nullptr;
  return static_cast<int>(exit_code);
}

void TerminateChild(ChildProcess& child)
{
  if (child.handle != nullptr) {
    TerminateProcess(static_cast<HANDLE>(child.handle), 1);
  }
}

void CloseChildProcess(ChildProcess& child)
{
  ClosePipe(child.to_child_write);
  ClosePipe(child.from_child_read);
  child.to_child_write = InvalidPipeHandle();
  child.from_child_read = InvalidPipeHandle();
  if (child.handle != nullptr) {
    CloseHandle(static_cast<HANDLE>(child.handle));
    child.handle = nullptr;
  }
}

} // namespace neko::platform
