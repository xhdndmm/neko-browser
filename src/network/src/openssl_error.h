#pragma once

// Shared OpenSSL error-queue helper for the network module.  Internal header:
// it is not installed and must not be included from public headers.

#include <openssl/err.h>
#include <string>

namespace neko::network {

// Collects and drains the OpenSSL error queue into a readable string.
inline std::string SslErrorString()
{
  std::string msg;
  unsigned long code = 0;
  while ((code = ERR_get_error()) != 0) {
    if (!msg.empty()) {
      msg += "; ";
    }
    msg += ERR_error_string(code, nullptr);
  }
  return msg.empty() ? "unknown TLS error" : msg;
}

} // namespace neko::network
