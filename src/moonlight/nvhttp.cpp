#include "moonlight/nvhttp.h"

#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "casement/log.h"
#include "casement/text.h"
#include "moonlight/options.h"

namespace moonlight {

using namespace std::chrono_literals;

namespace {

// moonlight-qt's timeouts.
constexpr auto request_timeout = 5s;
constexpr auto launch_timeout = 120s;
constexpr auto resume_timeout = 30s;

}  // namespace

std::string launch_arguments(const LaunchParams& p) {
    // The remote input key id is the IV's first four bytes as a signed
    // big-endian number, as Moonlight sends it.
    auto key_id = static_cast<std::int32_t>(static_cast<std::uint32_t>(p.ri_key_id[0]) << 24 |
                                            static_cast<std::uint32_t>(p.ri_key_id[1]) << 16 |
                                            static_cast<std::uint32_t>(p.ri_key_id[2]) << 8 | p.ri_key_id[3]);
    int fps = p.fps > 60 && p.nvidia_software ? 0 : p.fps;
    return std::format(
        "appid={}&mode={}x{}x{}&additionalStates=1&sops={}&rikey={}&rikeyid={}&localAudioPlayMode={}"
        "&surroundAudioInfo={}&remoteControllersBitmap=0&gcmap=0&gcpersist=0{}",
        p.app_id, p.width, p.height, fps, p.sops ? 1 : 0, casement::hex(p.ri_key), key_id, p.local_audio ? 1 : 0,
        p.surround_audio_info, p.extra);
}

NvHttp::NvHttp(std::string host, std::uint16_t http_port, const Identity& identity, http::Canceller& cancel)
    : host_(std::move(host)), http_port_(http_port), identity_(identity), cancel_(cancel) {}

std::string NvHttp::get(Channel channel, std::string_view command, const std::string& arguments,
                        std::chrono::milliseconds timeout, const std::string& pinned_pem, http::Canceller* cancel) {
    http::Canceller* canceller = cancel ? cancel : &cancel_;
    std::uint8_t uuid[16];
    openssl_random(uuid);
    http::Request request;
    request.target = std::format("/{}?uniqueid={}&uuid={}{}{}", command,
                                 nvidia_ ? std::string("0123456789ABCDEF") : identity_.unique_id(), casement::hex(uuid),
                                 arguments.empty() ? "" : "&", arguments);
    request.timeout = timeout;
    casement::log::debug("{} /{}", channel == Channel::https ? "HTTPS" : "HTTP", command);
    http::Response response;
    if (channel == Channel::https) {
        http::Tls tls;
        tls.certificate = identity_.certificate();
        tls.key = identity_.key();
        tls.pinned_pem = pinned_pem.empty() ? server_pem_ : pinned_pem;
        response = http::fetch(host_, https_port_ ? https_port_ : default_https_port, request, &tls, canceller);
    } else {
        response = http::fetch(host_, http_port_, request, nullptr, canceller);
    }
    // Pairing replies carry key material; the rest may be logged.
    if (command != "pair") casement::log::trace("{} answered {}: {}", command, response.status, response.body);
    return std::move(response.body);
}

ServerInfo NvHttp::serverinfo_http() {
    ServerInfo info = parse_serverinfo(get(Channel::http, "serverinfo", "", request_timeout));
    https_port_ = info.https_port ? info.https_port : default_https_port;
    return info;
}

ServerInfo NvHttp::serverinfo_https() {
    return parse_serverinfo(get(Channel::https, "serverinfo", "", request_timeout));
}

std::vector<App> NvHttp::applist() { return parse_applist(get(Channel::https, "applist", "", request_timeout)); }

std::string NvHttp::start_app(std::string_view verb, const LaunchParams& params) {
    return parse_launch(
        get(Channel::https, verb, launch_arguments(params), verb == "resume" ? resume_timeout : launch_timeout));
}

void NvHttp::cancel_app(std::chrono::milliseconds timeout, http::Canceller* cancel) {
    Reply::parse(get(Channel::https, "cancel", "", timeout, {}, cancel));
}

PairRequest NvHttp::pair_request() {
    return [this](Channel channel, std::string_view command, const std::string& arguments,
                  std::chrono::milliseconds timeout, const std::string& pinned_pem) {
        return get(channel, command, arguments, timeout, pinned_pem);
    };
}

}  // namespace moonlight
