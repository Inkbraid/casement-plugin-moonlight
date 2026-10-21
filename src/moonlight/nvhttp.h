// The GameStream control API as Moonlight uses it: /serverinfo, /pair,
// /applist, /launch, /resume and /cancel, over HTTP (the host's HTTP port,
// 47989 by default) before pairing and over HTTPS (47984) with the client
// certificate after it.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "moonlight/http.h"
#include "moonlight/identity.h"
#include "moonlight/pairing.h"
#include "moonlight/xml.h"

namespace moonlight {

/// What /launch and /resume need beyond the app.
struct LaunchParams {
    int app_id = 0;
    int width = 0;
    int height = 0;
    int fps = 0;
    /// The remote input key and the first four bytes of its IV, as in the
    /// stream configuration.
    std::array<std::uint8_t, 16> ri_key{};
    std::array<std::uint8_t, 4> ri_key_id{};
    /// Ask the host to change its display settings for the stream.
    bool sops = true;
    /// Play sound on the host as well.
    bool local_audio = false;
    /// SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION of the stream.
    int surround_audio_info = 0;
    /// GFE needs fps 0 above 60 fps to keep the resolution.
    bool nvidia_software = false;
    /// LiGetLaunchUrlQueryParameters().
    std::string extra;
};

/// The arguments of /launch and /resume, as moonlight-qt's
/// NvHTTP::startApp builds them (SDR, no gamepads).
std::string launch_arguments(const LaunchParams& p);

class NvHttp {
public:
    NvHttp(std::string host, std::uint16_t http_port, const Identity& identity, http::Canceller& cancel);

    /// The host's HTTPS port, from serverinfo.
    void set_https_port(std::uint16_t port) { https_port_ = port; }
    /// The host certificate to expect over HTTPS.
    void set_server_certificate(std::string pem) { server_pem_ = std::move(pem); }
    const std::string& server_certificate() const { return server_pem_; }
    /// GeForce Experience gets the placeholder unique ID all Moonlight
    /// clients share there, so they can quit each other's games.
    void set_nvidia_software(bool nvidia) { nvidia_ = nvidia; }

    /// GET /<command>?uniqueid=…&uuid=…&<arguments>; returns the body.
    /// `cancel` replaces the client's canceller for this request.
    std::string get(Channel channel, std::string_view command, const std::string& arguments,
                    std::chrono::milliseconds timeout, const std::string& pinned_pem = {},
                    http::Canceller* cancel = nullptr);

    /// Over HTTP; the HTTPS port comes from here.
    ServerInfo serverinfo_http();
    /// Over HTTPS with our certificate: reports whether we're paired.
    /// Throws HostError 401 when the host doesn't know our certificate and
    /// http::Error (certificate) when it isn't the pinned one.
    ServerInfo serverinfo_https();

    std::vector<App> applist();
    /// "launch" or "resume"; returns the RTSP session URL.
    std::string start_app(std::string_view verb, const LaunchParams& params);
    /// Ends the running app (the stream with it).
    void cancel_app(std::chrono::milliseconds timeout, http::Canceller* cancel = nullptr);

    /// For pairing::pair.
    PairRequest pair_request();

    const std::string& host() const { return host_; }

private:
    std::string host_;
    std::uint16_t http_port_;
    std::uint16_t https_port_ = 0;
    std::string server_pem_;
    bool nvidia_ = false;
    const Identity& identity_;
    http::Canceller& cancel_;
};

}  // namespace moonlight
