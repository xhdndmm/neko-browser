#include "neko/ipc/channel.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace neko::ipc {

namespace {

Channel::Handle InvalidHandle()
{
#ifdef _WIN32
  return INVALID_HANDLE_VALUE;
#else
  return -1;
#endif
}

void CloseRaw(Channel::Handle h)
{
#ifdef _WIN32
  CloseHandle(h);
#else
  ::close(h);
#endif
}

// Reads exactly |n| bytes into |buf|.  Returns false on EOF/error.  On a
// clean EOF before the first byte, sets |clean_eof| so the caller can
// distinguish "peer closed between frames" from "truncated frame".
bool ReadExactly(Channel::Handle h, uint8_t* buf, std::size_t n, bool* clean_eof)
{
  *clean_eof = false;
  std::size_t done = 0;
  while (done < n) {
#ifdef _WIN32
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf + done, static_cast<DWORD>(n - done), &got, nullptr);
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
#else
    const ssize_t got = ::read(h, buf + done, n - done);
    if (got == 0) {
      if (done == 0) {
        *clean_eof = true;
      }
      return false;
    }
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    done += static_cast<std::size_t>(got);
#endif
  }
  return true;
}

} // namespace

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
#ifdef _WIN32
  return h != nullptr && h != INVALID_HANDLE_VALUE;
#else
  return h >= 0;
#endif
}

Channel::Channel() : read_handle_(InvalidHandle()), write_handle_(InvalidHandle()) {}

Channel::Channel(Channel&& other) noexcept
    : read_handle_(other.read_handle_), write_handle_(other.write_handle_)
{
  other.read_handle_ = InvalidHandle();
  other.write_handle_ = InvalidHandle();
}

Channel& Channel::operator=(Channel&& other) noexcept
{
  if (this != &other) {
    Close();
    read_handle_ = other.read_handle_;
    write_handle_ = other.write_handle_;
    other.read_handle_ = InvalidHandle();
    other.write_handle_ = InvalidHandle();
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

bool Channel::open() const
{
  return HandleValid(read_handle_) || HandleValid(write_handle_);
}

void Channel::Close()
{
  if (HandleValid(read_handle_)) {
    CloseRaw(read_handle_);
    read_handle_ = InvalidHandle();
  }
  if (HandleValid(write_handle_)) {
    CloseRaw(write_handle_);
    write_handle_ = InvalidHandle();
  }
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
  std::size_t done = 0;
  while (done < frame.size()) {
#ifdef _WIN32
    DWORD wrote = 0;
    const BOOL ok = WriteFile(write_handle_,
                              frame.data() + done,
                              static_cast<DWORD>(frame.size() - done),
                              &wrote,
                              nullptr);
    if (!ok) {
      return false;
    }
    if (wrote == 0) {
      return false;
    }
    done += wrote;
#else
    const ssize_t wrote = ::write(write_handle_, frame.data() + done, frame.size() - done);
    if (wrote < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (wrote == 0) {
      return false;
    }
    done += static_cast<std::size_t>(wrote);
#endif
  }
  return true;
}

base::Result<std::string> Channel::Receive()
{
  if (!HandleValid(read_handle_)) {
    return base::Err(base::Error::Io("channel read end is closed"));
  }
  std::uint8_t header[4];
  bool clean_eof = false;
  if (!ReadExactly(read_handle_, header, 4, &clean_eof)) {
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
      !ReadExactly(
          read_handle_, reinterpret_cast<uint8_t*>(payload.data()), payload.size(), &clean_eof)) {
    return base::Err(base::Error::InvalidArgument("frame body is truncated"));
  }
  return payload;
}

} // namespace neko::ipc
