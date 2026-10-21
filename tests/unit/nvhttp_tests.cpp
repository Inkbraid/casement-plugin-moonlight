// The control API client: /launch arguments as Moonlight builds them, and
// the requests it sends.

#include "check.h"

#include "test_server.h"

#include <chrono>
#include <regex>
#include <string>
#include <vector>

#include "moonlight/nvhttp.h"

using namespace moonlight;
using namespace std::chrono_literals;

TEST(launch_arguments_like_moonlight) {
    LaunchParams p;
    p.app_id = 881448767;
    p.width = 1920;
    p.height = 1080;
    p.fps = 60;
    for (int i = 0; i < 16; ++i) p.ri_key[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    p.ri_key_id = {0x12, 0x34, 0x56, 0x78};
    p.surround_audio_info = 196610;  // stereo
    p.extra = "&corever=1";
    CHECK_EQ(launch_arguments(p),
             std::string("appid=881448767&mode=1920x1080x60&additionalStates=1&sops=1"
                         "&rikey=000102030405060708090a0b0c0d0e0f&rikeyid=305419896&localAudioPlayMode=0"
                         "&surroundAudioInfo=196610&remoteControllersBitmap=0&gcmap=0&gcpersist=0&corever=1"));
}

TEST(launch_arguments_key_id_is_signed) {
    LaunchParams p;
    p.app_id = 1;
    p.width = 1280;
    p.height = 720;
    p.fps = 30;
    p.ri_key_id = {0x87, 0x65, 0x43, 0x21};
    p.sops = false;
    p.local_audio = true;
    std::string a = launch_arguments(p);
    // moonlight-qt reads the IV's first four bytes as a big-endian int.
    CHECK(a.find("&rikeyid=-2023406815&") != std::string::npos);
    CHECK(a.find("&sops=0&") != std::string::npos);
    CHECK(a.find("&localAudioPlayMode=1&") != std::string::npos);
    CHECK(a.find("mode=1280x720x30&") != std::string::npos);
}

TEST(launch_arguments_gfe_above_60_fps) {
    LaunchParams p;
    p.width = 1920;
    p.height = 1080;
    p.fps = 120;
    CHECK(launch_arguments(p).find("mode=1920x1080x120&") != std::string::npos);
    p.nvidia_software = true;
    CHECK(launch_arguments(p).find("mode=1920x1080x0&") != std::string::npos);
}

TEST(nvhttp_request_format) {
    std::vector<std::string> requests;
    test::Server server([&](test::Conn& c) {
        requests.push_back(c.read_request());
        std::string body =
            R"(<root status_code="200"><hostname>h</hostname><appversion>7.1.431.-1</appversion><uniqueid>U</uniqueid><HttpsPort>47984</HttpsPort></root>)";
        c.write("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
    });
    Identity id = Identity::generate();
    http::Canceller cancel;
    NvHttp nv("127.0.0.1", server.port(), id, cancel);
    ServerInfo s = nv.serverinfo_http();
    CHECK_EQ(s.hostname, std::string("h"));
    nv.get(Channel::http, "pair", "devicename=roth&updateState=1", 5s);
    nv.set_nvidia_software(true);
    nv.get(Channel::http, "unpair", "", 5s);
    REQUIRE(requests.size() == 3);
    // Each request: the client's unique ID and a fresh random uuid.
    std::regex first("^GET /serverinfo\\?uniqueid=" + id.unique_id() + "&uuid=[0-9a-f]{32} HTTP/1\\.1\r\n");
    CHECK(std::regex_search(requests[0], first));
    std::regex second("^GET /pair\\?uniqueid=" + id.unique_id() +
                      "&uuid=[0-9a-f]{32}&devicename=roth&updateState=1 HTTP/1\\.1\r\n");
    CHECK(std::regex_search(requests[1], second));
    CHECK(requests[0].substr(0, 90) != requests[1].substr(0, 90));
    // GeForce Experience gets the ID every Moonlight client shares there.
    CHECK(requests[2].starts_with("GET /unpair?uniqueid=0123456789ABCDEF&uuid="));
    CHECK_EQ(id.unique_id().size(), std::size_t{16});
}
