#include "neko/ipc/subprocess.h"

#include <cerrno>
#include <cstring>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace neko::ipc {

namespace {

#ifdef _WIN32

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

#endif // _WIN32

} // namespace

Subprocess::Subprocess(Subprocess&& other) noexcept
    : channel_(std::move(other.channel_))
#ifdef _WIN32
      ,
      process_handle_(other.process_handle_)
#else
      ,
      pid_(other.pid_)
#endif
{
#ifdef _WIN32
  other.process_handle_ = nullptr;
#else
  other.pid_ = -1;
#endif
}

Subprocess& Subprocess::operator=(Subprocess&& other) noexcept
{
  if (this != &other) {
    CloseProcessHandle();
    channel_ = std::move(other.channel_);
#ifdef _WIN32
    process_handle_ = other.process_handle_;
    other.process_handle_ = nullptr;
#else
    pid_ = other.pid_;
    other.pid_ = -1;
#endif
  }
  return *this;
}

Subprocess::~Subprocess()
{
  CloseProcessHandle();
}

void Subprocess::CloseProcessHandle()
{
#ifdef _WIN32
  if (process_handle_ != nullptr) {
    CloseHandle(process_handle_);
    process_handle_ = nullptr;
  }
#else
  pid_ = -1;
#endif
}

base::Result<Subprocess> Subprocess::Spawn(const std::vector<std::string>& argv)
{
  if (argv.empty()) {
    return base::Err(base::Error::InvalidArgument("empty argv"));
  }

#ifdef _WIN32
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
  startup.hStdError = child_stdout_write;

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
  if (!ok) {
    CloseHandle(child_stdin_write);
    CloseHandle(child_stdout_read);
    return base::Err(base::Error::Io("CreateProcessW failed"));
  }
  CloseHandle(process.hThread);

  Subprocess spawned;
  spawned.process_handle_ = process.hProcess;
  spawned.channel_ = Channel::FromHandles(child_stdout_read, child_stdin_write);
  return spawned;
#else
  int to_child[2] = {-1, -1};
  int from_child[2] = {-1, -1};
  if (::pipe(to_child) != 0 || ::pipe(from_child) != 0) {
    const std::string reason = std::strerror(errno);
    if (to_child[0] >= 0) {
      ::close(to_child[0]);
      ::close(to_child[1]);
    }
    if (from_child[0] >= 0) {
      ::close(from_child[0]);
      ::close(from_child[1]);
    }
    return base::Err(base::Error::Io("pipe failed: " + reason));
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    const std::string reason = std::strerror(errno);
    ::close(to_child[0]);
    ::close(to_child[1]);
    ::close(from_child[0]);
    ::close(from_child[1]);
    return base::Err(base::Error::Io("fork failed: " + reason));
  }
  if (pid == 0) {
    // Child: attach the pipes to stdin/stdout, then exec.  execvp keeps the
    // descriptors across exec (no CLOEXEC on plain pipes).
    ::dup2(to_child[0], STDIN_FILENO);
    ::dup2(from_child[1], STDOUT_FILENO);
    ::close(to_child[0]);
    ::close(to_child[1]);
    ::close(from_child[0]);
    ::close(from_child[1]);
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
      args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);
    ::execvp(args[0], args.data());
    _exit(127); // exec failed
  }

  // Parent: keep its ends, close the child's.
  ::close(to_child[0]);
  ::close(from_child[1]);

  Subprocess spawned;
  spawned.pid_ = pid;
  spawned.channel_ = Channel::FromHandles(from_child[0], to_child[1]);
  return spawned;
#endif
}

int Subprocess::Wait()
{
#ifdef _WIN32
  if (process_handle_ == nullptr) {
    return -1;
  }
  WaitForSingleObject(process_handle_, INFINITE);
  DWORD exit_code = 0;
  if (!GetExitCodeProcess(process_handle_, &exit_code)) {
    CloseHandle(process_handle_);
    process_handle_ = nullptr;
    return -1;
  }
  CloseHandle(process_handle_);
  process_handle_ = nullptr;
  return static_cast<int>(exit_code);
#else
  if (pid_ < 0) {
    return -1;
  }
  int status = 0;
  while (::waitpid(pid_, &status, 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
    pid_ = -1;
    return -1;
  }
  pid_ = -1;
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return -WTERMSIG(status);
  }
  return -1;
#endif
}

void Subprocess::Terminate()
{
#ifdef _WIN32
  if (process_handle_ != nullptr) {
    TerminateProcess(process_handle_, 1);
  }
#else
  if (pid_ >= 0) {
    ::kill(pid_, SIGTERM);
  }
#endif
}

} // namespace neko::ipc
