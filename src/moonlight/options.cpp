#include "moonlight/options.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>

#include "casement/address.h"
#include "casement/log.h"

namespace moonlight {

namespace {

std::optional<int> to_int(std::string_view s) {
    int value = 0;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc() || end != s.data() + s.size()) return std::nullopt;
    return value;
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    return out;
}

}  // namespace

std::string_view to_string(Codec codec) {
    switch (codec) {
        case Codec::automatic: return "auto";
        case Codec::h264: return "h264";
        case Codec::hevc: return "hevc";
        case Codec::av1: return "av1";
    }
    return "auto";
}

std::string Options::url_host() const { return ipv6 ? "[" + host + "]" : host; }

std::optional<Options> parse_options(std::string_view address, std::string& error) {
    auto a = casement::parse_address(address, error);
    if (!a) return std::nullopt;
    auto bad = [&](std::string why) {
        error = std::move(why);
        return std::nullopt;
    };
    if (a->scheme != "moonlight") return bad(std::format("“{}” isn't a moonlight:// address.", address));

    Options o;
    o.host = a->host;
    o.ipv6 = a->ipv6;
    if (a->port) o.http_port = *a->port;
    if (!a->path.empty()) o.app = a->path;
    if (!a->user.empty()) casement::log::warn("ignoring the user name in the address: Sunshine has none");

    for (const auto& [name, value] : a->query) {
        if (name == "res") {
            std::string v = lower(value);
            if (v.empty() || v == "window") {
                o.window_size = true;
                continue;
            }
            auto x = v.find('x');
            auto w = x == std::string::npos ? std::nullopt : to_int(std::string_view(v).substr(0, x));
            auto h = x == std::string::npos ? std::nullopt : to_int(std::string_view(v).substr(x + 1));
            if (!w || !h || *w < 256 || *h < 144 || *w > 8192 || *h > 8192)
                return bad(std::format("“{}” isn't a resolution. Use “window” or a size such as 1920x1080.", value));
            // 4:2:0 video needs even sizes.
            o.window_size = false;
            o.width = *w & ~1;
            o.height = *h & ~1;
        } else if (name == "fps") {
            auto fps = to_int(value);
            if (!fps || *fps < 10 || *fps > 240)
                return bad(std::format("“{}” isn't a frame rate. Use a number from 10 to 240.", value));
            o.fps = *fps;
        } else if (name == "bitrate") {
            if (value.empty()) continue;
            auto kbps = to_int(value);
            if (!kbps || *kbps < 500 || *kbps > 500000)
                return bad(std::format("“{}” isn't a bitrate. Use kbit/s from 500 to 500000.", value));
            o.bitrate_kbps = *kbps;
        } else if (name == "codec") {
            std::string v = lower(value);
            if (v.empty() || v == "auto") o.codec = Codec::automatic;
            else if (v == "h264") o.codec = Codec::h264;
            else if (v == "hevc" || v == "h265") o.codec = Codec::hevc;
            else if (v == "av1") o.codec = Codec::av1;
            else return bad(std::format("“{}” isn't a codec. Use auto, h264, hevc or av1.", value));
        } else if (name == "quit") {
            o.quit = a->flag("quit");
        } else {
            casement::log::warn("ignoring the unknown address option “{}”", name);
        }
    }
    return o;
}

Size window_default_size(int screen_width, int screen_height) {
    if (screen_width < 256 || screen_height < 144) return {1920, 1080};
    int height = std::min(screen_height, 1080) & ~1;
    auto width = static_cast<int>(std::lround(static_cast<double>(screen_width) * height / screen_height));
    width = std::clamp(width, 256, 8192) / 8 * 8;
    return {width, height};
}

int default_bitrate_kbps(int width, int height, int fps) {
    // Frame rate isn't linear above 60 fps.
    double frame_rate_factor = (fps <= 60 ? fps : std::sqrt(fps / 60.0) * 60.0) / 30.0;
    struct Entry {
        long long pixels;
        double factor;
    };
    static constexpr Entry table[] = {{640 * 360, 1},    {854 * 480, 2},     {1280 * 720, 5},
                                      {1920 * 1080, 10}, {2560 * 1440, 20}, {3840 * 2160, 40}};
    long long pixels = static_cast<long long>(width) * height;
    double resolution_factor = table[std::size(table) - 1].factor;
    for (std::size_t i = 0; i < std::size(table); ++i) {
        if (pixels == table[i].pixels) {
            resolution_factor = table[i].factor;
            break;
        }
        if (pixels < table[i].pixels) {
            if (i == 0) {
                resolution_factor = table[0].factor;
            } else {
                const Entry& lo = table[i - 1];
                const Entry& hi = table[i];
                resolution_factor = static_cast<double>(pixels - lo.pixels) /
                                        static_cast<double>(hi.pixels - lo.pixels) * (hi.factor - lo.factor) +
                                    lo.factor;
            }
            break;
        }
    }
    return static_cast<int>(std::lround(resolution_factor * frame_rate_factor)) * 1000;
}

}  // namespace moonlight
