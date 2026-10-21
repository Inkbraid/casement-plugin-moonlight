// Addresses and the stream settings derived from them.

#include "check.h"

#include <cstdio>
#include <string_view>

#include "moonlight/options.h"

using namespace moonlight;

namespace {

Options parse(std::string_view address) {
    std::string error;
    auto o = parse_options(address, error);
    if (!o) std::fprintf(stderr, "%s: %s\n", std::string(address).c_str(), error.c_str());
    REQUIRE(o);
    return *o;
}

std::string error_of(std::string_view address) {
    std::string error;
    auto o = parse_options(address, error);
    CHECK(!o);
    CHECK(!error.empty());
    return error;
}

}  // namespace

TEST(options_spec_example) {
    Options o = parse("moonlight://gaming-pc.lan/Desktop?fps=60&codec=hevc");
    CHECK_EQ(o.host, std::string("gaming-pc.lan"));
    CHECK_EQ(o.http_port, std::uint16_t{47989});
    CHECK_EQ(o.app, std::string("Desktop"));
    CHECK_EQ(o.fps, 60);
    CHECK(o.codec == Codec::hevc);
    CHECK(o.window_size);
    CHECK_EQ(o.bitrate_kbps, 0);
    CHECK(!o.quit);
}

TEST(options_defaults) {
    Options o = parse("moonlight://10.0.0.5");
    CHECK_EQ(o.app, std::string("Desktop"));
    CHECK_EQ(o.fps, 60);
    CHECK(o.codec == Codec::automatic);
    CHECK(o.window_size);
    CHECK_EQ(o.url_host(), std::string("10.0.0.5"));
}

TEST(options_every_field) {
    Options o =
        parse("moonlight://[fd00::5]:48989/Steam%20Big%20Picture?res=2560x1440&fps=120&bitrate=40000&codec=h264&quit");
    CHECK(o.ipv6);
    CHECK_EQ(o.host, std::string("fd00::5"));
    CHECK_EQ(o.url_host(), std::string("[fd00::5]"));
    CHECK_EQ(o.http_port, std::uint16_t{48989});
    CHECK_EQ(o.app, std::string("Steam Big Picture"));
    CHECK(!o.window_size);
    CHECK_EQ(o.width, 2560);
    CHECK_EQ(o.height, 1440);
    CHECK_EQ(o.fps, 120);
    CHECK_EQ(o.bitrate_kbps, 40000);
    CHECK(o.codec == Codec::h264);
    CHECK(o.quit);
}

TEST(options_values) {
    CHECK(parse("moonlight://h?res=window").window_size);
    CHECK(parse("moonlight://h?res=").window_size);
    Options odd = parse("moonlight://h?res=1921X1081");
    CHECK_EQ(odd.width, 1920);
    CHECK_EQ(odd.height, 1080);
    CHECK(parse("moonlight://h?quit=1").quit);
    CHECK(!parse("moonlight://h?quit=0").quit);
    CHECK(parse("moonlight://h?codec=AUTO").codec == Codec::automatic);
    CHECK(parse("moonlight://h?codec=H265").codec == Codec::hevc);
    CHECK(parse("moonlight://h?codec=av1").codec == Codec::av1);
    CHECK_EQ(parse("moonlight://h?bitrate=").bitrate_kbps, 0);
    // Unknown options are ignored.
    CHECK_EQ(parse("moonlight://h?colour=blue&fps=30").fps, 30);
}

TEST(options_errors) {
    CHECK(error_of("vnc://host").find("moonlight://") != std::string::npos);
    CHECK(error_of("moonlight://host?res=big").find("resolution") != std::string::npos);
    CHECK(error_of("moonlight://host?res=100x100").find("resolution") != std::string::npos);
    CHECK(error_of("moonlight://host?fps=5").find("frame rate") != std::string::npos);
    CHECK(error_of("moonlight://host?fps=sixty").find("frame rate") != std::string::npos);
    CHECK(error_of("moonlight://host?bitrate=10").find("bitrate") != std::string::npos);
    CHECK(error_of("moonlight://host?codec=vp9").find("codec") != std::string::npos);
    error_of("moonlight://host:0");
    error_of("moonlight://");
}

TEST(options_window_size) {
    auto check = [](int sw, int sh, int w, int h) {
        Size s = window_default_size(sw, sh);
        CHECK_EQ(s.width, w);
        CHECK_EQ(s.height, h);
    };
    check(1920, 1080, 1920, 1080);
    check(2560, 1440, 1920, 1080);
    check(3840, 2160, 1920, 1080);
    check(3440, 1440, 2576, 1080);  // 21:9 keeps its shape
    check(1920, 1200, 1728, 1080);
    check(1366, 768, 1360, 768);
    check(1280, 1024, 1280, 1024);
    check(0, 0, 1920, 1080);  // no screen information
}

TEST(options_default_bitrate) {
    // moonlight-qt's StreamingPreferences::getDefaultBitrate.
    CHECK_EQ(default_bitrate_kbps(1920, 1080, 60), 20000);
    CHECK_EQ(default_bitrate_kbps(1920, 1080, 30), 10000);
    CHECK_EQ(default_bitrate_kbps(1280, 720, 60), 10000);
    CHECK_EQ(default_bitrate_kbps(2560, 1440, 60), 40000);
    CHECK_EQ(default_bitrate_kbps(3840, 2160, 60), 80000);
    CHECK_EQ(default_bitrate_kbps(1920, 1080, 120), 28000);
    CHECK_EQ(default_bitrate_kbps(1600, 900, 60), 15000);
    CHECK_EQ(default_bitrate_kbps(640, 360, 60), 2000);
    CHECK_EQ(default_bitrate_kbps(320, 240, 30), 1000);
    CHECK_EQ(default_bitrate_kbps(7680, 4320, 60), 80000);
}
