// Parsing the control API's replies, from Sunshine and GeForce Experience.

#include "check.h"

#include <string_view>

#include "moonlight/xml.h"

using namespace moonlight;

namespace {

// Captured from Sunshine 2026.516.143833 over HTTP.
constexpr std::string_view sunshine_serverinfo =
    R"(<?xml version="1.0" encoding="utf-8"?>
<root status_code="200"><hostname>casement-e2e</hostname><appversion>7.1.431.-1</appversion><GfeVersion>3.23.0.74</GfeVersion><uniqueid>DDC3ACA7-693E-AE2E-7D0F-280503CF5F8C</uniqueid><HttpsPort>47984</HttpsPort><ExternalPort>47989</ExternalPort><MaxLumaPixelsHEVC>0</MaxLumaPixelsHEVC><mac>00:00:00:00:00:00</mac><LocalIP>127.0.0.1</LocalIP><ServerCodecModeSupport>262401</ServerCodecModeSupport><PairStatus>0</PairStatus><currentgame>0</currentgame><state>SUNSHINE_SERVER_FREE</state></root>)";

// The shape of a GeForce Experience reply.
constexpr std::string_view gfe_serverinfo =
    R"(<?xml version="1.0" encoding="utf-8" standalone="yes"?>
<root protocol_version="0.1" query="serverinfo" status_code="200" status_message="OK">
<hostname>GAMING-PC</hostname>
<appversion>7.1.450.0</appversion>
<GfeVersion>3.23.0.74</GfeVersion>
<uniqueid>6CA9A9C4-6D97-B4E6-8A34-C0F7A2A0E1B2</uniqueid>
<HttpsPort>47984</HttpsPort>
<ExternalPort>47989</ExternalPort>
<PairStatus>1</PairStatus>
<currentgame>100021711</currentgame>
<state>MJOLNIR_STATE_SERVER_BUSY</state>
</root>)";

constexpr std::string_view sunshine_applist =
    R"(<?xml version="1.0" encoding="utf-8"?>
<root status_code="200"><App><IsHdrSupported>0</IsHdrSupported><AppTitle>Desktop</AppTitle><ID>881448767</ID></App><App><IsHdrSupported>0</IsHdrSupported><AppTitle>Low Res Desktop</AppTitle><ID>2050286458</ID></App><App><IsHdrSupported>1</IsHdrSupported><AppTitle>Steam Big Picture</AppTitle><ID>1093255277</ID></App><App><IsHdrSupported>0</IsHdrSupported><AppTitle/><ID>7</ID></App></root>)";

}  // namespace

TEST(xml_sunshine_serverinfo) {
    ServerInfo s = parse_serverinfo(sunshine_serverinfo);
    CHECK_EQ(s.hostname, std::string("casement-e2e"));
    CHECK_EQ(s.uniqueid, std::string("DDC3ACA7-693E-AE2E-7D0F-280503CF5F8C"));
    CHECK_EQ(s.appversion, std::string("7.1.431.-1"));
    CHECK_EQ(s.gfeversion, std::string("3.23.0.74"));
    CHECK_EQ(s.generation(), 7);
    CHECK_EQ(s.https_port, std::uint16_t{47984});
    CHECK_EQ(s.external_port, std::uint16_t{47989});
    CHECK_EQ(s.codec_support, 0x40101);
    CHECK(!s.paired);
    CHECK_EQ(s.current_game, 0);
    CHECK(!s.nvidia_software());
}

TEST(xml_gfe_serverinfo) {
    ServerInfo s = parse_serverinfo(gfe_serverinfo);
    CHECK(s.nvidia_software());
    CHECK(s.paired);
    CHECK_EQ(s.current_game, 100021711);
    // Hosts that don't say which codecs they have can stream H.264.
    CHECK_EQ(s.codec_support, 0x0001);
}

TEST(xml_current_game_only_when_busy) {
    std::string free(sunshine_serverinfo);
    free.replace(free.find("<currentgame>0"), 14, "<currentgame>881448767");
    CHECK_EQ(parse_serverinfo(free).current_game, 0);
    std::string busy = free;
    busy.replace(busy.find("SUNSHINE_SERVER_FREE"), 20, "SUNSHINE_SERVER_BUSY");
    CHECK_EQ(parse_serverinfo(busy).current_game, 881448767);
}

TEST(xml_errors) {
    try {
        Reply::parse(R"(<?xml version="1.0" encoding="utf-8"?>
<root status_code="401" query="" status_message="The client is not authorized. Certificate verification failed."/>)");
        CHECK(false);
    } catch (const HostError& e) {
        CHECK_EQ(e.code(), 401);
        CHECK_EQ(e.host_message(), std::string("The client is not authorized. Certificate verification failed."));
    }
    try {
        Reply::parse(R"(<root status_code="4294967295" status_message="Invalid"/>)");
        CHECK(false);
    } catch (const HostError& e) {
        CHECK_EQ(e.code(), -1);
    }
    bool threw = false;
    try {
        Reply::parse("<html><body>Not GameStream</body></html>");
    } catch (const ReplyError&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        parse_serverinfo(R"(<root status_code="200"><hostname>x</hostname></root>)");
    } catch (const ReplyError&) {
        threw = true;
    }
    CHECK(threw);
    CHECK_EQ(reply_status("not xml at all").code, 0);
    Status s = reply_status(R"(<root status_code="503" status_message="busy"/>)");
    CHECK_EQ(s.code, 503);
    CHECK_EQ(s.message, std::string("busy"));
}

TEST(xml_applist) {
    auto apps = parse_applist(sunshine_applist);
    REQUIRE(apps.size() == 4);
    CHECK_EQ(apps[0].title, std::string("Desktop"));
    CHECK_EQ(apps[0].id, 881448767);
    CHECK(apps[2].hdr);
    CHECK_EQ(apps[3].title, std::string(""));
    const App* a = find_app(apps, "desktop");
    REQUIRE(a);
    CHECK_EQ(a->id, 881448767);
    a = find_app(apps, "  Steam Big Picture ");
    REQUIRE(a);
    CHECK_EQ(a->id, 1093255277);
    CHECK(!find_app(apps, "Desk"));
    CHECK(!find_app(apps, "Cyberpunk 2077"));
    bool threw = false;
    try {
        parse_applist(R"(<root status_code="200"><App><AppTitle>No id</AppTitle></App></root>)");
    } catch (const ReplyError&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(parse_applist(R"(<root status_code="200"/>)").empty());
}

TEST(xml_launch) {
    CHECK_EQ(parse_launch(R"(<?xml version="1.0" encoding="utf-8"?>
<root status_code="200"><sessionUrl0>rtspenc://192.168.1.20:48010</sessionUrl0><gamesession>1</gamesession></root>)"),
             std::string("rtspenc://192.168.1.20:48010"));
    CHECK_EQ(parse_launch(R"(<root status_code="200"><resume>1</resume></root>)"), std::string(""));
    try {
        parse_launch(R"(<root status_code="400" status_message="An app is already running on this host"><resume>0</resume></root>)");
        CHECK(false);
    } catch (const HostError& e) {
        CHECK_EQ(e.code(), 400);
        CHECK_EQ(std::string(e.what()), std::string("An app is already running on this host"));
    }
}

TEST(xml_pair_fields) {
    Reply r = Reply::parse(R"(<root status_code="200"><paired>1</paired><plaincert>2d2d</plaincert></root>)");
    CHECK_EQ(r.get("paired"), std::string("1"));
    CHECK_EQ(r.get("plaincert"), std::string("2d2d"));
    CHECK(!r.text("challengeresponse"));
}
