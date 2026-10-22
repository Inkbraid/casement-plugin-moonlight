// What a moonlight:// address asks for: the fields of the manifest's
// connection type, checked and with their defaults filled in.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace moonlight {

enum class Codec { automatic, h264, hevc, av1 };

std::string_view to_string(Codec codec);

struct Options {
    std::string host;  // without IPv6 brackets
    bool ipv6 = false;
    std::uint16_t http_port = 47989;
    std::string app = "Desktop";
    /// res=window: protocol v1 has no initial view size, so the stream size
    /// is chosen from the screen (see window_default_size).
    bool window_size = true;
    int width = 0;
    int height = 0;
    int fps = 60;
    /// 0 when the address has none: default_bitrate_kbps then decides.
    int bitrate_kbps = 0;
    Codec codec = Codec::automatic;
    bool quit = false;

    /// The host as it goes into a URL or a Host header: IPv6 in brackets.
    std::string url_host() const;
};

inline constexpr std::uint16_t default_http_port = 47989;
inline constexpr std::uint16_t default_https_port = 47984;

/// Parses and checks an address. On failure `error` is a sentence for the
/// user.
std::optional<Options> parse_options(std::string_view address, std::string& error);

/// The stream size for res=window, from the size of the screen in pixels:
/// the screen's aspect ratio at no more than 1080 lines, both sides even
/// and the width a multiple of 8, which every encoder accepts.
struct Size {
    int width;
    int height;
};
Size window_default_size(int screen_width, int screen_height);

/// Moonlight's default bitrate for a stream (moonlight-qt's
/// StreamingPreferences::getDefaultBitrate for 4:2:0): 20 Mbit/s for
/// 1080p60, interpolated over resolution, sub-linear above 60 fps.
int default_bitrate_kbps(int width, int height, int fps);

}  // namespace moonlight
