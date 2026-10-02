#include "neko/browser/network_host.h"

#include "neko/base/logging.h"
#include "neko/browser/network_protocol.h"
#include "neko/ipc/channel.h"
#include "neko/network/http.h"
#include "neko/url/url.h"

#include <string>
#include <utility>
#include <vector>

namespace neko::browser {
namespace {

// Serves network requests for one browser process.  Single-threaded and
// synchronous by design (mirrors the browser's controller): one fetch at a
// time, with cookie lookups interleaved on the same channel while the fetch
// is in flight.
class NetworkHost
{
public:
  explicit NetworkHost(ipc::Channel channel) : channel_(std::move(channel)) {}

  int Run()
  {
    while (true) {
      auto frame = channel_.Receive();
      if (!frame.has_value()) {
        break; // The browser closed the pipe (or died): shut down.
      }
      auto request = DecodeNetworkRequest(frame.value());
      if (!request.has_value()) {
        // The framing is intact, so the stream stays usable; report the bad
        // payload and serve the next request.
        SendError("malformed network request: " + request.error().message());
        continue;
      }
      if (request.value().op == NetworkOp::kShutdown) {
        break;
      }
      HandleFetch(request.value());
    }
    return 0;
  }

private:
  void SendError(const std::string& message)
  {
    NetworkReply reply;
    reply.kind = NetworkReply::Kind::kError;
    reply.error = message;
    Send(reply);
  }

  bool Send(const NetworkReply& reply)
  {
    const auto encoded = EncodeNetworkReply(reply);
    return encoded.has_value() && channel_.Send(encoded.value());
  }

  void HandleFetch(const NetworkRequest& request)
  {
    const auto parsed = url::Url::Parse(request.url);
    if (!parsed.has_value()) {
      SendError("invalid URL: " + request.url);
      return;
    }

    bool browser_lost = false;
    bool first_hop = true;
    // HttpGet invokes this for every hop including the first.  |cookie_header|
    // (when the caller supplied one) is used for the first hop; redirect hops
    // ask the browser, because the jar and its HttpOnly values live there.
    network::HeaderProvider provider = [&](const url::Url& target) {
      std::vector<network::HttpHeader> headers;
      if (first_hop && !request.cookie_header.empty()) {
        first_hop = false;
        headers.push_back(network::HttpHeader{"cookie", request.cookie_header});
        return headers;
      }
      first_hop = false;

      NetworkReply lookup;
      lookup.kind = NetworkReply::Kind::kCookieLookup;
      lookup.lookup_url = target.Serialize();
      const auto encoded = EncodeNetworkReply(lookup);
      if (!encoded.has_value() || !channel_.Send(encoded.value())) {
        browser_lost = true;
        return headers;
      }
      auto reply_frame = channel_.Receive();
      if (!reply_frame.has_value()) {
        browser_lost = true;
        return headers;
      }
      auto cookie = DecodeCookieReply(reply_frame.value());
      if (!cookie.has_value()) {
        browser_lost = true;
        return headers;
      }
      if (!cookie.value().empty()) {
        headers.push_back(network::HttpHeader{"cookie", cookie.value()});
      }
      return headers;
    };

    auto response = network::HttpGet(parsed.value(), 5, provider);
    if (browser_lost) {
      // The browser vanished mid-fetch; its session is being torn down, so
      // report the failure and let the next Receive observe the closed pipe.
      SendError("browser closed during redirect cookie lookup");
      return;
    }
    if (!response.has_value()) {
      SendError(response.error().message());
      return;
    }
    if (response.value().body.size() > kMaxNetworkBodyBytes) {
      SendError("response body exceeds the network-process frame cap");
      return;
    }

    NetworkReply reply;
    reply.kind = NetworkReply::Kind::kResponse;
    reply.status_code = response.value().status_code;
    reply.reason = response.value().reason;
    reply.final_url = response.value().final_url;
    reply.headers.reserve(response.value().headers.size());
    for (const network::HttpHeader& header : response.value().headers) {
      reply.headers.push_back(NetworkReply::Header{header.name, header.value});
    }
    reply.body = std::move(response.value().body);
    Send(reply);
  }

  ipc::Channel channel_;
};

} // namespace

int RunNetworkChild()
{
  return NetworkHost(ipc::Channel::FromStdio()).Run();
}

} // namespace neko::browser
