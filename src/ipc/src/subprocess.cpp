#include "neko/ipc/subprocess.h"

#include <utility>

namespace neko::ipc {

Subprocess::Subprocess(Subprocess&& other) noexcept
    : channel_(std::move(other.channel_)), process_(other.process_)
{
  other.process_ = platform::ChildProcess{};
}

Subprocess& Subprocess::operator=(Subprocess&& other) noexcept
{
  if (this != &other) {
    platform::CloseChildProcess(process_);
    channel_ = std::move(other.channel_);
    process_ = other.process_;
    other.process_ = platform::ChildProcess{};
  }
  return *this;
}

Subprocess::~Subprocess()
{
  platform::CloseChildProcess(process_);
}

base::Result<Subprocess> Subprocess::Spawn(const std::vector<std::string>& argv)
{
  auto spawned = platform::SpawnProcessWithStdio(argv);
  if (!spawned.has_value()) {
    return base::Err(spawned.error());
  }
  Subprocess out;
  out.process_ = spawned.value();
  out.channel_ = Channel::FromHandles(out.process_.from_child_read, out.process_.to_child_write);
  return out;
}

long Subprocess::ProcessId() const
{
  return platform::ChildProcessId(process_);
}

int Subprocess::Wait()
{
  return platform::WaitForChild(process_);
}

void Subprocess::Terminate()
{
  platform::TerminateChild(process_);
}

} // namespace neko::ipc
