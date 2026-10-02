#pragma once

#include "neko/base/status.h"
#include "neko/browser/network_protocol.h"
#include "neko/ipc/subprocess.h"
#include "neko/network/http.h"
#include "neko/url/url.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace neko::browser {

// Browser-process side of the network process (ADR 0016 M3a).
//
// Owns one network child (the browser binary in --network-child mode) and
// speaks the network protocol over its stdio channel.  Every Fetch blocks
// until the child replies, which matches the controller's synchronous
// worker-thread design; DNS, TCP, TLS and HTTP/1.1 all run in the child's
// address space, so a crash there surfaces as a failed fetch instead of
// taking the browser down.
//
// Cookies stay browser-side: the child asks for the cookies of each hop
// (including redirects) through |lookup|, so HttpOnly cookies never exist in
// the child at all.
//
// Threading: not thread-safe; the owning controller uses it from its worker
// thread only.
class NetworkSession
{
public:
  // Returns the cookie header ("name=value; ...", no "Cookie:" prefix) for
  // |url|.  May be empty.  Invoked on the calling thread while the child
  // waits, once per hop.
  using CookieLookupFn = std::function<std::string(const url::Url&)>;

  // Spawns |executable| with --network-child.
  static base::Result<std::shared_ptr<NetworkSession>> Spawn(const std::string& executable);
  ~NetworkSession();

  NetworkSession(const NetworkSession&) = delete;
  NetworkSession& operator=(const NetworkSession&) = delete;

  // Fetches |url| through the child.  |cookie_header| is used for the first
  // hop when non-empty; every other hop's cookies come from |lookup|.
  base::Result<network::HttpResponse>
  Fetch(const url::Url& url, std::string_view cookie_header, const CookieLookupFn& lookup);

  // Sends kShutdown and reaps the child.  Safe to call more than once.
  void Shutdown();

  bool alive() const
  {
    return alive_;
  }
  // The child's process id (0 when not running).  For diagnostics and tests.
  long ProcessId() const
  {
    return child_.ProcessId();
  }

private:
  explicit NetworkSession(ipc::Subprocess child);
  base::Result<network::HttpResponse> RoundTrip(const NetworkRequest& request,
                                                const CookieLookupFn& lookup);

  ipc::Subprocess child_;
  bool alive_ = true;
};

} // namespace neko::browser
