#include "neko/browser/network_session.h"

#include <utility>
#include <vector>

namespace neko::browser {

NetworkSession::NetworkSession(ipc::Subprocess child) : child_(std::move(child)) {}

NetworkSession::~NetworkSession()
{
  Shutdown();
}

base::Result<std::shared_ptr<NetworkSession>> NetworkSession::Spawn(const std::string& executable)
{
  if (executable.empty()) {
    return base::Err(base::Error::InvalidArgument("no network executable"));
  }
  std::vector<std::string> argv = {executable, "--network-child", "--log-level", "warning"};
  auto spawned = ipc::Subprocess::Spawn(argv);
  if (!spawned.has_value()) {
    return base::Err(spawned.error());
  }
  return std::shared_ptr<NetworkSession>(new NetworkSession(std::move(spawned.value())));
}

void NetworkSession::Shutdown()
{
  if (!child_.channel().open()) {
    alive_ = false;
    return;
  }
  alive_ = false;
  NetworkRequest request;
  request.op = NetworkOp::kShutdown;
  const auto encoded = EncodeNetworkRequest(request);
  // Best-effort cooperative shutdown: the child replies by exiting, so the
  // reap below returns promptly.  If even the send fails the child is gone
  // (or unreachable) and Terminate reaps it.
  if (encoded.has_value() && child_.channel().Send(encoded.value())) {
    (void)child_.channel().Receive();
  } else {
    child_.Terminate();
  }
  (void)child_.Wait();
  child_.channel().Close();
}

base::Result<network::HttpResponse> NetworkSession::Fetch(const url::Url& url,
                                                          std::string_view cookie_header,
                                                          const CookieLookupFn& lookup)
{
  NetworkRequest request;
  request.op = NetworkOp::kFetch;
  request.url = url.Serialize();
  request.cookie_header.assign(cookie_header);
  return RoundTrip(request, lookup);
}

base::Result<network::HttpResponse> NetworkSession::RoundTrip(const NetworkRequest& request,
                                                              const CookieLookupFn& lookup)
{
  if (!alive_) {
    return base::Err(base::Error::Io("network process is not running"));
  }
  const auto encoded = EncodeNetworkRequest(request);
  if (!encoded.has_value()) {
    return base::Err(encoded.error());
  }
  if (!child_.channel().Send(encoded.value())) {
    alive_ = false;
    return base::Err(base::Error::Io("network process closed the connection"));
  }

  while (true) {
    auto frame = child_.channel().Receive();
    if (!frame.has_value()) {
      // A closed pipe or a malformed frame means the process is no longer
      // usable; the caller tears it down and can spawn a fresh one.
      alive_ = false;
      return base::Err(frame.error());
    }
    auto reply = DecodeNetworkReply(frame.value());
    if (!reply.has_value()) {
      alive_ = false;
      return base::Err(reply.error());
    }

    if (reply.value().kind == NetworkReply::Kind::kCookieLookup) {
      // The child is mid-fetch and needs this hop's cookies.  An empty cookie
      // string is a valid answer (no cookies for that host).
      std::string cookie;
      const auto parsed = url::Url::Parse(reply.value().lookup_url);
      if (lookup && parsed.has_value()) {
        cookie = lookup(parsed.value());
      }
      const auto encoded_cookie = EncodeCookieReply(cookie);
      if (!encoded_cookie.has_value() || !child_.channel().Send(encoded_cookie.value())) {
        alive_ = false;
        return base::Err(base::Error::Io("network process closed during cookie lookup"));
      }
      continue;
    }

    if (reply.value().kind == NetworkReply::Kind::kError) {
      const std::string& message = reply.value().error;
      return base::Err(base::Error::Io(message.empty() ? "network process error" : message));
    }

    network::HttpResponse response;
    response.status_code = reply.value().status_code;
    response.reason = reply.value().reason;
    response.final_url = reply.value().final_url;
    response.headers.reserve(reply.value().headers.size());
    for (const NetworkReply::Header& header : reply.value().headers) {
      response.headers.push_back(network::HttpHeader{header.name, header.value});
    }
    response.body = std::move(reply.value().body);
    return response;
  }
}

} // namespace neko::browser
