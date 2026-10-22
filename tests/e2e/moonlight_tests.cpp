// The plugin against a real Sunshine host, through Casement's side of the
// protocol (tools/sim): pairing with the PIN from the tab, a wrong PIN,
// the stream's picture, input reaching the host, sound, codecs, resume,
// the default stream size, quit, the host ending the stream and a host
// that forgot the client.
//
// ci/e2e.sh starts Sunshine (on an Xvfb screen of one known colour) and
// describes it in these variables:
//
//   MOONLIGHT_E2E_HOST            the host's address
//   MOONLIGHT_E2E_WEB_USER        Sunshine web UI user and password, to
//   MOONLIGHT_E2E_WEB_PASSWORD    enter PINs as a user would
//   MOONLIGHT_E2E_COLOUR          the screen's colour, rrggbb
//   MOONLIGHT_E2E_SUNSHINE_LOG    Sunshine's log (debug level): proof the
//                                 host received the input
//   MOONLIGHT_E2E_DATA            a fresh folder for the plugin's data
//   MOONLIGHT_E2E_HEVC=1          the host can encode HEVC
//   MOONLIGHT_E2E_AUDIO=1         the host captures a tone and the plugin
//                                 has an output device
//
// Without MOONLIGHT_E2E_HOST the tests skip, unless CASEMENT_E2E=1.

#include "check.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "casement/text.h"
#include "moonlight/http.h"
#include "moonlight/options.h"
#include "sim.h"

using namespace casement::sim;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

namespace {

std::wstring plugin_path() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return (std::filesystem::path(self).parent_path() / PLUGIN_EXE).wstring();
}

std::string env(const char* name) {
    char buf[4096];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) ? std::string(buf, n) : std::string();
}

struct Setup {
    std::string host;
    std::string user;
    std::string password;
    std::uint32_t colour = 0;  // 0xRRGGBB
    std::string sunshine_log;
    std::string data;
    bool hevc = false;
    bool audio = false;
};

/// The host ci/e2e.sh started, or nothing (the test then skips).
std::optional<Setup> setup() {
    Setup s;
    s.host = env("MOONLIGHT_E2E_HOST");
    if (s.host.empty()) {
        if (env("CASEMENT_E2E") == "1") {
            std::fprintf(stderr, "MOONLIGHT_E2E_HOST isn't set; ci/e2e.sh sets it\n");
            CHECK(false);
        } else {
            std::fprintf(stderr, "no Sunshine host configured (MOONLIGHT_E2E_HOST): skipped\n");
        }
        return std::nullopt;
    }
    s.user = env("MOONLIGHT_E2E_WEB_USER");
    s.password = env("MOONLIGHT_E2E_WEB_PASSWORD");
    s.colour = static_cast<std::uint32_t>(std::strtoul(env("MOONLIGHT_E2E_COLOUR").c_str(), nullptr, 16));
    s.sunshine_log = env("MOONLIGHT_E2E_SUNSHINE_LOG");
    s.data = env("MOONLIGHT_E2E_DATA");
    s.hevc = env("MOONLIGHT_E2E_HEVC") == "1";
    s.audio = env("MOONLIGHT_E2E_AUDIO") == "1";
    return s;
}

/// Sunshine's web API, used the way a user uses the web UI.
class Web {
public:
    explicit Web(const Setup& s) : s_(s) {}

    casement::sim::json post(const std::string& path, const std::string& body = "{}") {
        moonlight::http::Request r;
        r.method = "POST";
        r.target = path;
        r.body = body;
        std::string credentials = s_.user + ":" + s_.password;
        r.headers = {{"Authorization", "Basic " + casement::base64_encode(std::span(
                                                      reinterpret_cast<const std::uint8_t*>(credentials.data()),
                                                      credentials.size()))},
                     {"Content-Type", "application/json"}};
        moonlight::http::Tls tls;
        tls.any_server = true;  // the web UI's self-signed certificate
        auto response = moonlight::http::fetch(s_.host, 47990, r, &tls);
        return casement::sim::json::parse(response.body, nullptr, false);
    }

    bool pin(const std::string& pin) {
        auto j = post("/api/pin", casement::sim::json{{"pin", pin}, {"name", "casement-e2e"}}.dump());
        return j.is_object() && j.value("status", false);
    }

private:
    const Setup& s_;
};

/// The host's serverinfo over plain HTTP, as anyone may read it.
std::string serverinfo(const Setup& s) {
    moonlight::http::Request r;
    r.target = "/serverinfo?uniqueid=0123456789ABCDEF&uuid=00000000000000000000000000000000";
    return moonlight::http::fetch(s.host, 47989, r).body;
}

int current_game(const Setup& s) {
    std::smatch m;
    std::string info = serverinfo(s);
    if (std::regex_search(info, m, std::regex("<currentgame>(\\d+)</currentgame>"))) return std::stoi(m[1].str());
    return -1;
}

/// The plugin as Casement runs it, with the surfaces and acknowledgements
/// Casement provides.
class Plugin {
public:
    Plugin(const Setup& s, const std::string& address) {
        c_ = Casement::spawn(plugin_path(), {}, true,
                             {{"CASEMENT_MOONLIGHT_LOG", "debug"}, {"CASEMENT_MOONLIGHT_DATA", s.data}});
        REQUIRE(c_);
        REQUIRE(c_->hello());
        c_->connect(address);
    }

    /// Handles the plugin's messages until `done()` or the time is up.
    bool run_until(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
        auto end = clk::now() + timeout;
        while (!done()) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - clk::now());
            if (left.count() <= 0) return false;
            auto m = c_->recv(std::min<std::chrono::milliseconds>(left, 200ms));
            if (!m) {
                if (c_->wait(0ms)) return done();
                continue;
            }
            handle(*m);
        }
        return true;
    }

    /// Handles messages for a while.
    void run_for(std::chrono::milliseconds time) {
        run_until([] { return false; }, time);
    }

    /// The PIN the plugin shows, from its log.
    std::string pin() {
        std::string found;
        run_until(
            [&] {
                for (const auto& line : c_->stderr_lines())
                    if (auto p = line.find("pairing PIN "); p != std::string::npos) found = line.substr(p + 12, 4);
                return !found.empty();
            },
            30s);
        return found;
    }

    bool wait_state(std::string_view want, std::chrono::milliseconds timeout) {
        return run_until([&] { return state == want; }, timeout);
    }

    bool wait_frames(int count, std::chrono::milliseconds timeout) {
        int start = frames;
        return run_until([&] { return frames >= start + count; }, timeout);
    }

    void send(const json& m) { c_->send(m); }

    void disconnect() {
        c_->send(json{{"type", "disconnect"}});
        c_->close_stdin();
    }

    std::optional<DWORD> exit_code(std::chrono::milliseconds timeout) {
        run_until([&] { return c_->wait(0ms).has_value(); }, timeout);
        return c_->wait(0ms);
    }

    bool logged(std::string_view text) {
        for (const auto& line : c_->stderr_lines())
            if (line.find(text) != std::string::npos) return true;
        return false;
    }

    std::string log_line(std::string_view text) {
        for (const auto& line : c_->stderr_lines())
            if (line.find(text) != std::string::npos) return line;
        return {};
    }

    /// RGB of the last picture at (x, y).
    std::uint32_t rgb(std::uint32_t x, std::uint32_t y) const {
        const std::uint8_t* p = picture.data() + (std::size_t(y) * width + x) * 4;
        return std::uint32_t{p[2]} << 16 | std::uint32_t{p[1]} << 8 | p[0];
    }

    std::string state;
    std::string state_text;
    std::vector<std::pair<int, int>> surface_sizes;
    std::optional<json> cursor;
    int frames = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> picture;  // the last frame, BGRX, tightly packed

private:
    void handle(const json& m) {
        std::string type = m.value("type", "");
        if (type == "surface_request") {
            int w = m["width"], h = m["height"];
            surface_sizes.emplace_back(w, h);
            auto [s, msg] =
                c_->create_surface(++next_id_, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 64);
            REQUIRE(s);
            surface_ = std::move(s);
            c_->send(msg);
        } else if (type == "frame") {
            ++frames;
            if (surface_ && m.value("surface", 0) == static_cast<int>(next_id_)) {
                width = surface_->width;
                height = surface_->height;
                picture.resize(std::size_t(width) * height * 4);
                for (std::uint32_t y = 0; y < height; ++y)
                    std::memcpy(picture.data() + std::size_t(y) * width * 4,
                                surface_->pixels() + std::size_t(y) * surface_->stride, std::size_t(width) * 4);
            }
            c_->send(json{{"type", "frame_ack"}, {"surface", m["surface"]}});
        } else if (type == "state") {
            state = m.value("state", "");
            state_text = m.value("text", "");
        } else if (type == "cursor") {
            cursor = m;
        }
    }

    std::unique_ptr<Casement> c_;
    std::unique_ptr<HostSurface> surface_;
    std::uint32_t next_id_ = 0;
};

bool similar(std::uint32_t a, std::uint32_t b, int tolerance = 8) {
    for (int shift : {0, 8, 16}) {
        int x = static_cast<int>((a >> shift) & 0xFF), y = static_cast<int>((b >> shift) & 0xFF);
        if (std::abs(x - y) > tolerance) return false;
    }
    return true;
}

/// The stream shows the host's screen: one colour, away from the pointer
/// Sunshine draws into the picture.
void check_picture(const Plugin& p, const Setup& s) {
    REQUIRE(!p.picture.empty());
    for (auto [x, y] : {std::pair{40u, 40u}, {p.width - 40, 40u}, {40u, p.height - 40}, {p.width - 40, p.height - 40},
                        {p.width / 4, p.height / 4}}) {
        if (!similar(p.rgb(x, y), s.colour))
            std::fprintf(stderr, "pixel (%u, %u) is %06x, the host's screen %06x\n", x, y, p.rgb(x, y), s.colour);
        CHECK(similar(p.rgb(x, y), s.colour));
    }
}

/// Sunshine's log since `from` (its size before the test).
std::string sunshine_log(const Setup& s, std::size_t from) {
    std::ifstream in(s.sunshine_log, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    std::string all = text.str();
    return from < all.size() ? all.substr(from) : std::string();
}

std::size_t sunshine_log_size(const Setup& s) {
    std::error_code ec;
    auto n = std::filesystem::file_size(s.sunshine_log, ec);
    return ec ? 0 : static_cast<std::size_t>(n);
}

std::string address(const Setup& s, const std::string& extra = "") {
    return "moonlight://" + s.host + "/Desktop?res=1280x720&fps=30" + extra;
}

/// Starts a connection that needs pairing: the PIN screen, then the PIN.
std::string expect_pin_screen(Plugin& p) {
    CHECK(p.run_until([&] { return p.frames >= 1; }, 30s));
    REQUIRE(!p.surface_sizes.empty());
    CHECK((p.surface_sizes.front() == std::pair{960, 540}));
    // Text on the dark background: the screen isn't empty.
    REQUIRE(p.width == 960);
    int bright = 0;
    for (std::uint32_t y = 150; y < 350; ++y)
        for (std::uint32_t x = 0; x < p.width; ++x)
            if ((p.rgb(x, y) & 0xFF) > 200) ++bright;
    CHECK(bright > 2000);
    CHECK(p.state.empty());  // not connected while pairing
    std::string pin = p.pin();
    CHECK_EQ(pin.size(), std::size_t{4});
    return pin;
}

}  // namespace

TEST(e2e_wrong_pin_fails) {
    auto s = setup();
    if (!s) return;
    Web web(*s);
    Plugin p(*s, address(*s));
    std::string pin = expect_pin_screen(p);
    std::string wrong = std::to_string((std::stoi(pin) + 1) % 10000);
    wrong.insert(0, 4 - wrong.size(), '0');
    CHECK(web.pin(wrong));
    CHECK(p.wait_state("failed", 30s));
    CHECK(p.state_text.find("PIN") != std::string::npos);
    CHECK(p.state_text.find("wrong") != std::string::npos);
    auto code = p.exit_code(5s);
    REQUIRE(code);
    CHECK_EQ(*code, 1ul);
}

TEST(e2e_pairs_and_streams) {
    auto s = setup();
    if (!s) return;
    Web web(*s);
    std::size_t log_start = sunshine_log_size(*s);
    Plugin p(*s, address(*s));
    std::string pin = expect_pin_screen(p);
    // Right after a wrong PIN this works at once: the plugin ended the
    // host's earlier attempt.
    CHECK(web.pin(pin));
    CHECK(p.wait_state("connected", 60s));
    REQUIRE(p.state == "connected");
    // The invisible cursor, since Sunshine draws its own into the picture.
    CHECK(p.run_until([&] { return p.cursor.has_value(); }, 5s));
    REQUIRE(p.cursor);
    CHECK_EQ((*p.cursor)["width"].get<int>(), 1);
    CHECK_EQ((*p.cursor)["rgba"].get<std::string>(), std::string("AAAAAA=="));
    CHECK(p.wait_frames(30, 30s));
    CHECK((p.surface_sizes.back() == std::pair{1280, 720}));
    CHECK_EQ(p.width, 1280u);
    check_picture(p, *s);

    // Input: pointer, buttons, wheel and keys, as Casement sends them.
    p.send(json{{"type", "pointer"}, {"x", 100}, {"y", 200}, {"buttons", 0}});
    p.send(json{{"type", "pointer"}, {"x", 100}, {"y", 200}, {"buttons", 1}});
    p.send(json{{"type", "pointer"}, {"x", 100}, {"y", 200}, {"buttons", 0}});
    p.send(json{{"type", "pointer"}, {"x", 640}, {"y", 360}, {"buttons", 4}});
    p.send(json{{"type", "pointer"}, {"x", 640}, {"y", 360}, {"buttons", 0}});
    p.send(json{{"type", "wheel"}, {"delta", -2}});
    for (int sc : {0x2A, 0x1E}) p.send(json{{"type", "key"}, {"scancode", sc}, {"down", true}});
    for (int sc : {0x1E, 0x2A}) p.send(json{{"type", "key"}, {"scancode", sc}, {"down", false}});
    p.send(json{{"type", "key"}, {"scancode", 0x148}, {"down", true}});
    p.send(json{{"type", "key"}, {"scancode", 0x148}, {"down", false}});
    p.run_for(3s);

    if (!s->sunshine_log.empty()) {
        // Sunshine logs every input packet it decodes at debug level.
        std::string log = sunshine_log(*s, log_start);
        auto has = [&](const std::string& pattern) {
            bool found = std::regex_search(log, std::regex(pattern));
            if (!found) std::fprintf(stderr, "not in Sunshine's log: %s\n", pattern.c_str());
            return found;
        };
        // moonlight-common-c sends the reference size less one (for GFE's
        // scaling), and button actions one up for hosts of generation 5+:
        // 8 pressed, 9 released.
        CHECK(has("x +\\[100\\]\\s+y +\\[200\\]\\s+width +\\[1279\\]\\s+height +\\[719\\]"));
        CHECK(has("x +\\[640\\]\\s+y +\\[360\\]\\s+width +\\[1279\\]\\s+height +\\[719\\]"));
        CHECK(has("action \\[0*8\\]\\s+button \\[0*1\\][\\s\\S]*action \\[0*9\\]\\s+button \\[0*1\\]"));  // left click
        CHECK(has("action \\[0*8\\]\\s+button \\[0*3\\][\\s\\S]*action \\[0*9\\]\\s+button \\[0*3\\]"));  // right click
        CHECK(has("scrollAmt1 \\[-240\\]"));
        CHECK(has("keyCode \\[(80A0|A080)\\]\\s+modifiers \\[0*1\\]"));    // left Shift
        CHECK(has("keyCode \\[(8041|4180)\\]\\s+modifiers \\[0*1\\]"));    // Shift+A
        CHECK(has("keyCode \\[(8026|2680)\\]\\s+modifiers \\[0*10\\]"));   // Up, extended
    }

    if (s->audio) {
        // Muting pauses the output and the stream goes on.
        p.send(json{{"type", "mute"}, {"muted", true}});
        p.run_for(1500ms);
        CHECK(p.logged("audio: output paused"));
        int before = p.frames;
        p.send(json{{"type", "mute"}, {"muted", false}});
        p.run_for(1500ms);
        CHECK(p.logged("audio: output resumed"));
        CHECK(p.frames > before);
    }

    p.disconnect();
    auto code = p.exit_code(5s);
    REQUIRE(code);
    CHECK_EQ(*code, 0ul);
    if (s->audio) {
        // The host's test tone came through and was played.
        std::smatch m;
        std::string stats = p.log_line("packets decoded");
        REQUIRE(std::regex_search(stats, m,
                                  std::regex("(\\d+) packets decoded, (\\d+) frames played, peak level ([0-9.]+)")));
        CHECK(std::stoull(m[1].str()) > 100);
        CHECK(std::stoull(m[2].str()) > 48000);
        CHECK(std::stod(m[3].str()) > 0.1);
    }
}

TEST(e2e_second_connection_needs_no_pin) {
    auto s = setup();
    if (!s) return;
    Plugin p(*s, address(*s));
    CHECK(p.wait_state("connected", 30s));
    CHECK(p.wait_frames(10, 20s));
    // Straight to the stream: no PIN screen.
    REQUIRE(!p.surface_sizes.empty());
    CHECK((p.surface_sizes.front() == std::pair{1280, 720}));
    CHECK(!p.logged("pairing PIN"));
    // The app from the last connection was still running: resumed.
    CHECK(p.logged("resuming “Desktop”"));
    check_picture(p, *s);
    p.disconnect();
    CHECK(p.exit_code(5s) == std::optional<DWORD>(0));
}

TEST(e2e_window_size_by_default) {
    auto s = setup();
    if (!s) return;
    // Without res the plugin picks a size from the screen, as Windows
    // reports it here (protocol v1 doesn't tell it the view's size).
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    REQUIRE(EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode));
    moonlight::Size want = moonlight::window_default_size(static_cast<int>(mode.dmPelsWidth),
                                                          static_cast<int>(mode.dmPelsHeight));
    Plugin p(*s, "moonlight://" + s->host + "/Desktop?fps=30");
    CHECK(p.wait_state("connected", 30s));
    CHECK(p.wait_frames(10, 20s));
    REQUIRE(!p.surface_sizes.empty());
    CHECK((p.surface_sizes.back() == std::pair{want.width, want.height}));
    CHECK(p.logged(std::format("res=window: {}x{}", want.width, want.height)));
    // Sunshine scales its 16:9 screen into the stream keeping its shape, so
    // only the middle row is sure to show it.
    for (std::uint32_t x : {p.width / 4, p.width * 3 / 4}) CHECK(similar(p.rgb(x, p.height / 2), s->colour));
    p.disconnect();
    CHECK(p.exit_code(5s) == std::optional<DWORD>(0));
}

TEST(e2e_hevc) {
    auto s = setup();
    if (!s) return;
    if (!s->hevc) {
        std::fprintf(stderr, "the host can't encode HEVC: skipped\n");
        return;
    }
    Plugin p(*s, address(*s, "&codec=hevc"));
    CHECK(p.wait_state("connected", 30s));
    CHECK(p.wait_frames(20, 30s));
    CHECK(p.logged("video: HEVC 1280x720"));
    CHECK(p.logged("decoding hevc"));
    check_picture(p, *s);
    p.disconnect();
    CHECK(p.exit_code(5s) == std::optional<DWORD>(0));
}

TEST(e2e_unknown_app_fails) {
    auto s = setup();
    if (!s) return;
    Plugin p(*s, "moonlight://" + s->host + "/No%20Such%20App");
    CHECK(p.wait_state("failed", 30s));
    CHECK(p.state_text.find("no app called “No Such App”") != std::string::npos);
    CHECK(p.state_text.find("“Desktop”") != std::string::npos);
    CHECK(p.exit_code(5s) == std::optional<DWORD>(1));
}

TEST(e2e_quit_ends_the_app) {
    auto s = setup();
    if (!s) return;
    {
        Plugin p(*s, address(*s, "&quit"));
        CHECK(p.wait_state("connected", 30s));
        // Long enough for Sunshine to see the stream fully running: it
        // answers /cancel only once its sessions have ended.
        CHECK(p.wait_frames(90, 30s));
        CHECK(current_game(*s) > 0);
        p.disconnect();
        CHECK(p.exit_code(5s) == std::optional<DWORD>(0));
        CHECK(p.logged("HTTPS /cancel"));
    }
    auto end = clk::now() + 15s;
    while (current_game(*s) != 0 && clk::now() < end) std::this_thread::sleep_for(250ms);
    CHECK_EQ(current_game(*s), 0);
    // Without quit the app stays for the next connection.
    {
        Plugin p(*s, address(*s));
        CHECK(p.wait_state("connected", 30s));
        CHECK(p.logged("starting “Desktop”"));
        p.disconnect();
        CHECK(p.exit_code(5s) == std::optional<DWORD>(0));
    }
    CHECK(current_game(*s) > 0);
}

TEST(e2e_host_ends_the_stream) {
    auto s = setup();
    if (!s) return;
    Web web(*s);
    Plugin p(*s, address(*s));
    CHECK(p.wait_state("connected", 30s));
    CHECK(p.wait_frames(5, 20s));
    // The user quits the app on the host.
    web.post("/api/apps/close");
    CHECK(p.wait_state("closed", 30s));
    CHECK(p.state_text.find("ended the stream") != std::string::npos);
    CHECK(p.exit_code(5s) == std::optional<DWORD>(0));
}

TEST(e2e_forgotten_client_pairs_again) {
    auto s = setup();
    if (!s) return;
    Web web(*s);
    // The host's owner removes all paired devices.
    web.post("/api/clients/unpair-all");
    Plugin p(*s, address(*s));
    std::string pin = expect_pin_screen(p);
    CHECK(p.logged("no longer knows this client"));
    CHECK(web.pin(pin));
    CHECK(p.wait_state("connected", 60s));
    CHECK(p.wait_frames(10, 20s));
    check_picture(p, *s);
    p.disconnect();
    CHECK(p.exit_code(5s) == std::optional<DWORD>(0));
}

CHECK_MAIN
