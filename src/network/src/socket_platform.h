#pragma once

// Internal platform shim for the neko::network module.
//
// Everything that differs between Winsock and POSIX sockets lives behind these
// functions (and in socket_platform.cpp), so socket.cpp / dns.cpp /
// tls_socket.cpp read the same on every platform and the platform headers stay
// in one place:
//
//   * header selection (winsock2.h / ws2tcpip.h vs. the POSIX socket headers),
//   * WSAStartup (Winsock requires a process-wide initialization step),
//   * descriptor close / non-blocking mode / readiness waiting
//     (closesocket + ioctlsocket + select vs. close + fcntl + poll),
//   * error codes (WSAGetLastError vs. errno; WSAEWOULDBLOCK vs. EINPROGRESS),
//   * numeric address parsing and formatting
//     (InetPtonA/InetNtopA vs. inet_pton/inet_ntop).
//
// Length arguments are the other visible difference: Winsock takes plain ints
// for sockaddr lengths while POSIX uses socklen_t, hence platform::SockLen.
// This header is internal to the module; it is not installed and must not be
// included from public headers.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#ifdef _WIN32
// Winsock2 must be included before windows.h (which may arrive through some
// other header); WIN32_LEAN_AND_MEAN keeps the Windows headers small.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

namespace neko::network::platform {

#ifdef _WIN32
// Winsock length arguments are ints; POSIX uses socklen_t.
using SockLen = int;
#else
using SockLen = socklen_t;
#endif

// Winsock needs a process-wide WSAStartup() before any socket call; POSIX
// needs nothing.  Idempotent, thread-safe, and lazily initialized; returns
// false when the platform networking stack could not be initialized.
bool EnsureInitialized();

// Closes |fd|.  No-op for a negative descriptor, so callers may call it
// unconditionally before overwriting a member.
void CloseSocket(int fd);

// Creates a socket.  Returns the descriptor, or -1 on failure (LastError).
int CreateSocket(int family, int type, int protocol);

// Switches |fd| between non-blocking and (default) blocking mode.  Non-blocking
// mode is used to bound connect() by the caller's timeout.
bool SetNonBlocking(int fd, bool enabled);

// Bounds blocking sends/receives with SO_RCVTIMEO / SO_SNDTIMEO.
bool SetSocketTimeouts(int fd, int timeout_ms);

// Connects |fd| to |address|.  Returns 0 on success, -1 on failure
// (LastError).
int ConnectSocket(int fd, const struct sockaddr* address, SockLen length);

// Waits until |fd| becomes readable and/or writable.  A negative |timeout_ms|
// waits indefinitely.  Returns 1 when ready, 0 on timeout, -1 on error
// (LastError).
int WaitSocket(int fd, bool want_read, bool want_write, int timeout_ms);

// The SO_ERROR value a completed non-blocking connect left behind (0 when the
// connection succeeded), or -1 when the query itself failed.
int PendingSocketError(int fd);

// Sends |length| bytes; returns the byte count, or -1 on failure (LastError).
int SendBytes(int fd, const char* data, std::size_t length);

// Receives up to |length| bytes; returns the byte count (0 = orderly
// shutdown), or -1 on failure (LastError).
int ReceiveBytes(int fd, char* data, std::size_t length);

// The error code of the most recent failed socket call.
int LastError();

// True when |error| means "the non-blocking operation is still in progress"
// (EINPROGRESS on POSIX, WSAEWOULDBLOCK under Winsock).
bool IsInProgress(int error);

// True when |error| means the call was interrupted by a signal (EINTR /
// WSAEINTR); the caller retries.
bool IsInterrupted(int error);

// Human-readable text for a socket error code.
std::string ErrorText(int error);

// Human-readable text for a getaddrinfo()/getnameinfo() return code.
std::string GaiErrorText(int error);

// Parses a numeric address.  |out| receives the address in network byte order
// (4 or 16 bytes).  Returns false when |text| is not a numeric address of that
// family.
bool ParseIpv4(std::string_view text, std::uint8_t out[4]);
bool ParseIpv6(std::string_view text, std::uint8_t out[16]);

// Formats a numeric address (IPv6 in the compressed RFC 5952 form, matching
// inet_ntop).  Returns an empty string on failure.
std::string FormatIpv4(const std::uint8_t bytes[4]);
std::string FormatIpv6(const std::uint8_t bytes[16]);

} // namespace neko::network::platform
