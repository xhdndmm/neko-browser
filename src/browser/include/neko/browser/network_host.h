#pragma once

namespace neko::browser {

// Network child entry point (ADR 0016 M3a): serves the network protocol on
// stdin/stdout until the browser sends kShutdown or closes the pipe, then
// exits.  DNS, TCP, TLS and HTTP/1.1 run in this address space; cookies are
// resolved by asking the browser for each hop's cookie header, so the cookie
// jar (and HttpOnly values) never move into the child.
//
// Returns the process exit code.
int RunNetworkChild();

} // namespace neko::browser
