#include "neko/ipc/channel.h"

#include <cstdint>
#include <utility>

namespace neko::ipc {

base::Result<std::string> EncodeFrame(std::string_view payload)
{
  if (payload.size() > kMaxFrameBytes) {
    return base::Err(base::Error::InvalidArgument("frame payload exceeds the cap"));
  }
  const std::uint32_t length = static_cast<std::uint32_t>(payload.size());
  std::string frame;
  frame.reserve(payload.size() + 4);
  frame.push_back(static_cast<char>(length & 0xff));
  frame.push_back(static_cast<char>((length >> 8) & 0xff));
  frame.push_back(static_cast<char>((length >> 16) & 0xff));
  frame.push_back(static_cast<char>((length >> 24) & 0xff));
  frame.append(payload.data(), payload.size());
  return frame;
}

base::Result<std::string> DecodeFrame(std::string_view frame)
{
  if (frame.size() < 4) {
    return base::Err(base::Error::InvalidArgument("truncated frame header"));
  }
  const auto byte = [&](std::size_t i) -> std::uint32_t {
    return static_cast<std::uint32_t>(static_cast<uint8_t>(frame[i]));
  };
  const std::uint64_t length = byte(0) | (byte(1) << 8) | (byte(2) << 16) | (byte(3) << 24);
  if (length > kMaxFrameBytes) {
    return base::Err(base::Error::InvalidArgument("frame length exceeds the cap"));
  }
  if (frame.size() != 4 + length) {
    return base::Err(base::Error::InvalidArgument("frame body is truncated"));
  }
  return std::string(frame.data() + 4, static_cast<std::size_t>(length));
}

bool Channel::HandleValid(Handle h)
{
  return platform::PipeHandleValid(h);
}

Channel::Channel()
    : read_handle_(platform::InvalidPipeHandle()), write_handle_(platform::InvalidPipeHandle())
{}

Channel::Channel(Channel&& other) noexcept
    : read_handle_(other.read_handle_), write_handle_(other.write_handle_)
{
  other.read_handle_ = platform::InvalidPipeHandle();
  other.write_handle_ = platform::InvalidPipeHandle();
}

Channel& Channel::operator=(Channel&& other) noexcept
{
  if (this != &other) {
    Close();
    read_handle_ = other.read_handle_;
    write_handle_ = other.write_handle_;
    other.read_handle_ = platform::InvalidPipeHandle();
    other.write_handle_ = platform::InvalidPipeHandle();
  }
  return *this;
}

Channel::~Channel()
{
  Close();
}

Channel Channel::FromHandles(Handle read_handle, Handle write_handle)
{
  Channel channel;
  channel.read_handle_ = read_handle;
  channel.write_handle_ = write_handle;
  return channel;
}

Channel Channel::FromStdio()
{
  const platform::PipePair pipes = platform::StandardIoPipes();
  return FromHandles(pipes.read, pipes.write);
}

bool Channel::open() const
{
  return HandleValid(read_handle_) || HandleValid(write_handle_);
}

void Channel::Close()
{
  platform::ClosePipe(read_handle_);
  platform::ClosePipe(write_handle_);
  read_handle_ = platform::InvalidPipeHandle();
  write_handle_ = platform::InvalidPipeHandle();
}

bool Channel::Send(const std::string& payload)
{
  if (!HandleValid(write_handle_)) {
    return false;
  }
  const auto encoded = EncodeFrame(payload);
  if (!encoded.has_value()) {
    return false;
  }
  const std::string& frame = encoded.value();
  return platform::PipeWriteAll(
      write_handle_, reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size());
}

base::Result<std::string> Channel::Receive()
{
  if (!HandleValid(read_handle_)) {
    return base::Err(base::Error::Io("channel read end is closed"));
  }
  std::uint8_t header[4];
  bool clean_eof = false;
  if (!platform::PipeReadExactly(read_handle_, header, 4, &clean_eof)) {
    if (clean_eof) {
      return base::Err(base::Error::Io("peer closed"));
    }
    return base::Err(base::Error::Io("read failed"));
  }
  const std::uint64_t length =
      static_cast<std::uint64_t>(header[0]) | (static_cast<std::uint64_t>(header[1]) << 8) |
      (static_cast<std::uint64_t>(header[2]) << 16) | (static_cast<std::uint64_t>(header[3]) << 24);
  if (length > kMaxFrameBytes) {
    return base::Err(base::Error::InvalidArgument("frame length exceeds the cap"));
  }
  std::string payload(static_cast<std::size_t>(length), '\0');
  if (length > 0 &&
      !platform::PipeReadExactly(
          read_handle_, reinterpret_cast<uint8_t*>(payload.data()), payload.size(), &clean_eof)) {
    return base::Err(base::Error::InvalidArgument("frame body is truncated"));
  }
  return payload;
}

} // namespace neko::ipc
