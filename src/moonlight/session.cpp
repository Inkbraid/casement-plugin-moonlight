#include "moonlight/session.h"

#include <windows.h>

#include <openssl/rand.h>

#include <Limelight.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "casement/log.h"
#include "casement/text.h"
#include "moonlight/audio.h"
#include "moonlight/frames.h"
#include "moonlight/identity.h"
#include "moonlight/keymap.h"
#include "moonlight/nvhttp.h"
#include "moonlight/pairing.h"
#include "moonlight/screen.h"
#include "moonlight/video.h"

namespace moonlight {

namespace {

using namespace std::chrono_literals;
using casement::json;
using casement::State;

/// The user disconnected: the worker unwinds without reporting anything.
struct Stopped {};

constexpr auto disconnect_budget = 1800ms;  // Casement ends the process after 2 s
constexpr auto quit_timeout = 1200ms;

std::string ports_hint(unsigned flags) {
    if (flags == 0) return {};
    char ports[160] = {};
    LiStringifyPortFlags(flags, ", ", ports, sizeof(ports));
    return std::format(" Check that the host's ports {} can be reached from here.", ports);
}

std::string termination_text(int error, const std::string& host) {
    switch (error) {
        case ML_ERROR_NO_VIDEO_TRAFFIC:
            return std::format("No video arrived from {}.", host) +
                   ports_hint(LiGetPortFlagsFromTerminationErrorCode(error));
        case ML_ERROR_NO_VIDEO_FRAME:
            return std::format(
                "The connection to {} is too poor for whole video frames to arrive; a lower bitrate may help.", host);
        case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
            return std::format("{} ended the stream right after it started. It may be showing protected content, or "
                               "its display may be off.",
                               host);
        case ML_ERROR_PROTECTED_CONTENT:
            return std::format("{} ended the stream because protected content is on its screen.", host);
        case ML_ERROR_FRAME_CONVERSION:
            return std::format("{} ended the stream because it couldn't convert the picture for streaming.", host);
        default:
            return std::format("The connection to {} was lost (error {}).", host, error);
    }
}

Size primary_screen_size() {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsWidth && mode.dmPelsHeight)
        return {static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight)};
    return {GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

std::string quoted_list(const std::vector<App>& apps) {
    std::string out;
    for (const App& a : apps) out += std::format("{}“{}”", out.empty() ? "" : ", ", a.title);
    return out.empty() ? "none" : out;
}

class MoonlightSession;

/// moonlight-common-c keeps one connection per process and its callbacks
/// carry no context.
MoonlightSession* active = nullptr;

class MoonlightSession final : public casement::Session {
public:
    MoonlightSession(casement::Host& host, Options options) : host_(host), options_(std::move(options)) {
        active = this;
        worker_ = std::thread([this] { run(); });
    }

    ~MoonlightSession() override {
        bool done;
        {
            std::lock_guard g(lock_);
            done = done_;
        }
        if (done) {
            worker_.join();
        } else {
            // Still busy in the library after the disconnect budget: leave
            // the rest to process exit.
            casement::log::warn("the connection didn't close in time; exiting");
            ExitProcess(0);
        }
    }

    void input(const json& msg) override {
        std::string type = casement::get_string(msg, "type");
        if (type == "mute") {
            bool muted = casement::get_bool(msg, "muted");
            casement::log::info("sound {}", muted ? "muted" : "on");
            audio_.set_muted(muted);
            return;
        }
        // Input before the stream runs (the PIN screen) goes nowhere.
        if (!streaming_) return;
        std::lock_guard g(input_lock_);
        if (type == "key") {
            auto scancode = static_cast<std::uint32_t>(casement::get_int(msg, "scancode"));
            if (auto e = keyboard_.key(scancode, casement::get_bool(msg, "down")))
                LiSendKeyboardEvent2(e->key_code, e->action, e->modifiers, e->flags);
            else
                casement::log::trace("key {:#x} not sent", scancode);
        } else if (type == "pointer") {
            pointer(static_cast<int>(casement::get_int(msg, "x")), static_cast<int>(casement::get_int(msg, "y")),
                    static_cast<int>(casement::get_int(msg, "buttons")));
        } else if (type == "wheel") {
            auto delta = casement::get_int(msg, "delta");
            while (delta != 0) {
                auto step = std::clamp<std::int64_t>(delta, -127, 127);
                LiSendScrollEvent(static_cast<signed char>(step));
                delta -= step;
            }
        }
    }

    void disconnect() override {
        {
            std::lock_guard g(lock_);
            if (stopping_) return;
            stopping_ = true;
            if (starting_) LiInterruptConnection();
        }
        cv_.notify_all();
        cancel_.cancel();
        std::unique_lock g(lock_);
        if (!cv_.wait_for(g, disconnect_budget, [&] { return done_; }))
            casement::log::warn("closing the connection is taking long");
    }

    // moonlight-common-c callbacks.
    static int dr_setup(int format, int width, int height, int fps, void*, int) {
        casement::log::info("video: {} {}x{} at {} fps", format & VIDEO_FORMAT_MASK_H265 ? "HEVC" : "H.264", width,
                            height, fps);
        return active->video_->setup(format, width, height) ? 0 : -1;
    }
    static int dr_submit(PDECODE_UNIT unit) { return active->video_->submit(unit); }
    static void dr_cleanup() { active->video_->cleanup(); }
    static int ar_init(int, const POPUS_MULTISTREAM_CONFIGURATION config, void*, int) {
        return active->audio_.init(*config);
    }
    static void ar_cleanup() { active->audio_.cleanup(); }
    static void ar_play(char* data, int length) { active->audio_.play(data, length); }
    static void cl_stage_starting(int stage) { casement::log::debug("{}", LiGetStageName(stage)); }
    static void cl_stage_complete(int stage) {
        // Pictures can show before the connection counts as started; input
        // works from here on.
        if (stage == STAGE_INPUT_STREAM_START) active->streaming_ = true;
    }
    static void cl_stage_failed(int stage, int error) {
        casement::log::warn("{} failed: {}", LiGetStageName(stage), error);
        active->failed_stage_ = stage;
        active->failed_error_ = error;
    }
    static void cl_started() { active->started(); }
    static void cl_terminated(int error) { active->terminated(error); }
    static void cl_status(int status) {
        casement::log::info("connection quality: {}", status == CONN_STATUS_POOR ? "poor" : "okay");
    }
    static void cl_log(const char* format, ...) {
        char buf[1024];
        va_list args;
        va_start(args, format);
        std::vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        std::string_view line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
        if (!line.empty()) casement::log::info("moonlight: {}", line);
    }

private:
    bool stopping() {
        std::lock_guard g(lock_);
        return stopping_;
    }

    /// Reports the failure and ends the process; while disconnecting, just
    /// unwinds.
    [[noreturn]] void failed(const std::string& text) {
        if (stopping()) throw Stopped{};
        host_.finish(State::failed, text);
    }

    void check_stop(const http::Error& e) {
        if (e.kind() == http::Error::Kind::cancelled || stopping()) throw Stopped{};
    }

    /// Runs a control request, turning its errors into a failure that says
    /// what was being done.
    template <class F>
    auto request(std::string_view doing, F&& f) -> decltype(f()) {
        try {
            return f();
        } catch (const http::Error& e) {
            check_stop(e);
            failed(std::format("{}: {}", doing, e.what()));
        } catch (const HostError& e) {
            failed(std::format("{}: {}", doing, e.what()));
        } catch (const ReplyError& e) {
            failed(std::format("{}: {}", doing, e.what()));
        }
    }

    void show(const Screen& screen) {
        sink_.set_size(screen_width, screen_height);
        sink_.write_frame([&](std::uint8_t* bgrx, std::size_t stride) {
            draw_screen(screen, bgrx, screen_width, screen_height, stride);
        });
    }

    void run() {
        try {
            stream();
        } catch (const Stopped&) {
            casement::log::info("disconnected");
        } catch (const std::exception& e) {
            if (!stopping()) host_.finish(State::failed, e.what());
        }
        {
            std::lock_guard g(lock_);
            done_ = true;
        }
        cv_.notify_all();
    }

    void stream() {
        const std::string& host = options_.host;
        if (options_.codec == Codec::av1)
            failed("AV1 streams aren't supported by this version of the plugin. Choose Automatic, H.264 or HEVC.");

        std::filesystem::path folder;
        std::optional<Identity> identity;
        try {
            folder = data_folder();
            identity.emplace(Identity::load(folder));
        } catch (const std::exception& e) {
            failed(std::format("The client certificate can't be set up: {}", e.what()));
        }
        HostStore hosts(folder);
        NvHttp nv(host, options_.http_port, *identity, cancel_);

        // The host's details over HTTP, then over HTTPS if we have its
        // certificate: only that tells whether it still knows ours.
        ServerInfo info;
        try {
            info = nv.serverinfo_http();
        } catch (const http::Error& e) {
            check_stop(e);
            if (e.kind() == http::Error::Kind::resolve) failed(e.what());
            failed(std::format("{} Is Sunshine running there, on HTTP port {}?", e.what(), options_.http_port));
        } catch (const std::exception& e) {
            failed(std::format("{} doesn't answer like a GameStream host: {}", host, e.what()));
        }
        nv.set_nvidia_software(info.nvidia_software());
        casement::log::info("{} is “{}”, version {}, codecs {:#x}", host, info.hostname, info.appversion,
                            info.codec_support);

        bool paired = false;
        if (auto pem = hosts.certificate(info.uniqueid)) {
            nv.set_server_certificate(*pem);
            try {
                ServerInfo secure;
                try {
                    secure = nv.serverinfo_https();
                } catch (const http::Error& e) {
                    // A host refusing our certificate may close before its
                    // answer is read; ask once more before giving up.
                    if (e.kind() != http::Error::Kind::io && e.kind() != http::Error::Kind::protocol) throw;
                    casement::log::info("HTTPS serverinfo: {}; trying again", e.what());
                    secure = nv.serverinfo_https();
                }
                if (secure.paired) {
                    info = secure;
                    paired = true;
                }
            } catch (const HostError& e) {
                if (e.code() != 401) failed(std::format("{} refused the connection: {}", host, e.what()));
                casement::log::info("{} no longer knows this client; pairing again", host);
            } catch (const http::Error& e) {
                check_stop(e);
                if (e.kind() != http::Error::Kind::certificate) failed(e.what());
                casement::log::warn("{}'s certificate changed since pairing; pairing again", host);
            } catch (const ReplyError& e) {
                failed(std::format("{}: {}", host, e.what()));
            }
        }
        bool paired_now = false;
        if (!paired) {
            pair(nv, *identity, info, hosts);
            info = request("The paired connection doesn't work", [&] { return nv.serverinfo_https(); });
            paired_now = true;
        }

        auto apps = request(std::format("Can't get the list of apps on {}", host), [&] { return nv.applist(); });
        const App* app = find_app(apps, options_.app);
        if (!app)
            failed(std::format("{} has no app called “{}”. Its apps: {}.", host, options_.app, quoted_list(apps)));
        app_ = app->title;

        // Codecs: what was asked for, or H.264 plus HEVC when the GPU
        // decodes HEVC (software HEVC costs more than software H.264).
        HardwareDecoding hw = probe_hardware_decoding();
        int formats = VIDEO_FORMAT_H264;
        if (options_.codec == Codec::hevc) {
            if (!(info.codec_support & SCM_HEVC))
                failed(std::format("{} can't stream HEVC. Choose H.264 or Automatic.", host));
            formats = VIDEO_FORMAT_H265;
        } else if (options_.codec == Codec::automatic && hw.hevc && (info.codec_support & SCM_HEVC)) {
            formats |= VIDEO_FORMAT_H265;
        }
        bool hardware = formats & VIDEO_FORMAT_H265 ? hw.hevc : hw.h264;

        Size size{options_.width, options_.height};
        if (options_.window_size) {
            Size screen = primary_screen_size();
            size = window_default_size(screen.width, screen.height);
            casement::log::info("res=window: {}x{} (protocol v1 doesn't tell the view's size)", size.width,
                                size.height);
        }
        STREAM_CONFIGURATION config;
        LiInitializeStreamConfiguration(&config);
        config.width = size.width;
        config.height = size.height;
        config.fps = options_.fps;
        config.bitrate = options_.bitrate_kbps ? options_.bitrate_kbps
                                               : default_bitrate_kbps(size.width, size.height, options_.fps);
        config.packetSize = 1392;
        config.streamingRemotely = STREAM_CFG_AUTO;
        config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
        config.supportedVideoFormats = formats;
        config.colorSpace = COLORSPACE_REC_709;
        config.colorRange = COLOR_RANGE_LIMITED;
        config.encryptionFlags = static_cast<int>(ENCFLG_ALL);
        // Only the IV's first four bytes are used (they become rikeyid).
        RAND_bytes(reinterpret_cast<unsigned char*>(config.remoteInputAesKey), sizeof(config.remoteInputAesKey));
        RAND_bytes(reinterpret_cast<unsigned char*>(config.remoteInputAesIv), 4);

        if (info.current_game != 0 && info.current_game != app->id) {
            std::string running = "another app";
            for (const App& a : apps)
                if (a.id == info.current_game) running = std::format("“{}”", a.title);
            failed(std::format("{} is already streaming {}. Quit it on the host, or connect to that app instead.", host,
                               running));
        }
        bool resume = info.current_game == app->id;
        if (paired_now)
            show(Screen{L"Paired with " + casement::widen(host), L"",
                        (resume ? L"Resuming “" : L"Starting “") + casement::widen(app_) + L"”…", L""});

        LaunchParams launch;
        launch.app_id = app->id;
        launch.width = config.width;
        launch.height = config.height;
        launch.fps = config.fps;
        std::copy_n(reinterpret_cast<const std::uint8_t*>(config.remoteInputAesKey), 16, launch.ri_key.begin());
        std::copy_n(reinterpret_cast<const std::uint8_t*>(config.remoteInputAesIv), 4, launch.ri_key_id.begin());
        launch.surround_audio_info = SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(config.audioConfiguration);
        launch.nvidia_software = info.nvidia_software();
        launch.extra = LiGetLaunchUrlQueryParameters();
        casement::log::info("{} “{}” on {} at {}x{}, {} fps, {} kbit/s", resume ? "resuming" : "starting", app_, host,
                            config.width, config.height, config.fps, config.bitrate);
        std::string session_url;
        try {
            session_url = request(std::format("{} couldn't {} “{}”", host, resume ? "resume" : "start", app_),
                                  [&] { return nv.start_app(resume ? "resume" : "launch", launch); });
        } catch (const Stopped&) {
            // The host may have started the app anyway.
            if (options_.quit && !resume) quit(nv);
            throw;
        }

        SERVER_INFORMATION server;
        LiInitializeServerInformation(&server);
        server.address = host.c_str();
        server.serverInfoAppVersion = info.appversion.c_str();
        server.serverInfoGfeVersion = info.gfeversion.empty() ? nullptr : info.gfeversion.c_str();
        server.rtspSessionUrl = session_url.empty() ? nullptr : session_url.c_str();
        server.serverCodecModeSupport = info.codec_support;

        CONNECTION_LISTENER_CALLBACKS listener;
        LiInitializeConnectionCallbacks(&listener);
        listener.stageStarting = cl_stage_starting;
        listener.stageComplete = cl_stage_complete;
        listener.stageFailed = cl_stage_failed;
        listener.connectionStarted = cl_started;
        listener.connectionTerminated = cl_terminated;
        listener.logMessage = cl_log;
        listener.connectionStatusUpdate = cl_status;

        DECODER_RENDERER_CALLBACKS decoder;
        LiInitializeVideoCallbacks(&decoder);
        decoder.setup = dr_setup;
        decoder.cleanup = dr_cleanup;
        decoder.submitDecodeUnit = dr_submit;
        // As Moonlight: the software decoder works on slices in parallel; both
        // cope with HEVC reference frame invalidation.
        int slices = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));
        decoder.capabilities = CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC |
                               (hardware ? 0 : static_cast<int>(CAPABILITY_SLICES_PER_FRAME(slices)));
        video_ = std::make_unique<VideoDecoder>(sink_, hardware);

        AUDIO_RENDERER_CALLBACKS audio;
        LiInitializeAudioCallbacks(&audio);
        audio.init = ar_init;
        audio.cleanup = ar_cleanup;
        audio.decodeAndPlaySample = ar_play;

        {
            std::lock_guard g(lock_);
            if (stopping_) {
                if (options_.quit) quit(nv);
                throw Stopped{};
            }
            starting_ = true;
        }
        int err = LiStartConnection(&server, &config, &listener, &decoder, &audio, nullptr, 0, nullptr, 0);
        {
            std::lock_guard g(lock_);
            starting_ = false;
        }
        if (err != 0) {
            if (stopping()) {
                if (options_.quit) quit(nv);
                throw Stopped{};
            }
            if (failed_stage_ != STAGE_NONE)
                failed(std::format("The stream from {} couldn't start: {} failed (error {}).{}", host,
                                   LiGetStageName(failed_stage_), failed_error_,
                                   ports_hint(LiGetPortFlagsFromStage(failed_stage_))));
            failed(std::format("The stream from {} couldn't start (error {}).", host, err));
        }

        // Streaming until Casement disconnects or the stream ends.
        std::unique_lock g(lock_);
        cv_.wait(g, [&] { return stopping_ || terminated_; });
        bool stop = stopping_;
        int error = termination_error_;
        g.unlock();

        release_input();
        streaming_ = false;
        LiStopConnection();
        casement::log::info("{} frames decoded ({})", video_->frames(), video_->hardware() ? "hardware" : "software");
        if (stop) {
            if (options_.quit) quit(nv);
            throw Stopped{};
        }
        if (error == ML_ERROR_GRACEFUL_TERMINATION)
            host_.finish(State::closed, std::format("{} ended the stream.", host));
        host_.finish(State::failed, termination_text(error, host));
    }

    void pair(NvHttp& nv, const Identity& identity, const ServerInfo& info, HostStore& hosts) {
        const std::string& host = options_.host;
        std::string pin = random_pin();
        casement::log::info("pairing with {}: the PIN is in the tab", host);
        // The PIN is on screen anyway; tests read it from here.
        casement::log::debug("pairing PIN {}", pin);
        show(pin_screen(pin, host));

        PairClient client{identity.certificate_pem(), identity.certificate(), identity.key()};
        PairResult r;
        try {
            r = moonlight::pair(nv.pair_request(), client, info.generation(), pin);
        } catch (const http::Error& e) {
            check_stop(e);
            if (e.kind() == http::Error::Kind::timeout)
                failed(std::format("Nobody entered the PIN on {} in time. Connect again for a new PIN.", host));
            failed(std::format("Pairing with {} failed: {}", host, e.what()));
        }
        switch (r.outcome) {
            case PairResult::Outcome::paired:
                break;
            case PairResult::Outcome::wrong_pin:
                failed(std::format("The PIN entered on {} was wrong. Connect again to get a new PIN.", host));
            case PairResult::Outcome::already_in_progress:
                failed(std::format("{} is already pairing with another device. Finish that first, then connect again.",
                                   host));
            case PairResult::Outcome::failed:
                failed(std::format("Pairing with {} failed. {}", host, r.message));
        }
        try {
            hosts.pin(info.uniqueid, r.certificate_pem, info.hostname,
                      std::format("{}:{}", options_.url_host(), options_.http_port));
        } catch (const std::exception& e) {
            // Streaming works now; the next connection will ask for a PIN.
            casement::log::error("can't save the pairing: {}", e.what());
        }
        nv.set_server_certificate(r.certificate_pem);
        casement::log::info("paired with {}", host);
    }

    /// Ends the app on the host (the `quit` option). The disconnect has
    /// cancelled the session's requests, so this one gets its own.
    void quit(NvHttp& nv) {
        http::Canceller own;
        try {
            nv.cancel_app(quit_timeout, &own);
            casement::log::info("quit “{}” on {}", app_, options_.host);
        } catch (const http::Error& e) {
            // Sunshine answers once its stream sessions have ended, which
            // can take longer than Casement waits; it quits the app anyway.
            if (e.kind() == http::Error::Kind::timeout)
                casement::log::info("asked {} to quit “{}”; no answer yet", options_.host, app_);
            else
                casement::log::warn("can't quit “{}” on {}: {}", app_, options_.host, e.what());
        } catch (const std::exception& e) {
            casement::log::warn("can't quit “{}” on {}: {}", app_, options_.host, e.what());
        }
    }

    void started() {
        host_.send(casement::state_message(State::connected));
        // Sunshine draws the pointer into the picture; an invisible local
        // pointer avoids showing two.
        static constexpr std::array<std::uint8_t, 4> transparent{0, 0, 0, 0};
        host_.send(casement::cursor_message(1, 1, 0, 0, transparent));
        streaming_ = true;
        casement::log::info("streaming “{}” from {}", app_, options_.host);
    }

    void terminated(int error) {
        casement::log::info("the stream ended: {}", error);
        {
            std::lock_guard g(lock_);
            terminated_ = true;
            termination_error_ = error;
        }
        cv_.notify_all();
    }

    void pointer(int x, int y, int buttons) {
        int w = sink_.width(), h = sink_.height();
        if (w <= 0 || h <= 0) return;
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        if (x != x_ || y != y_) {
            LiSendMousePositionEvent(static_cast<short>(x), static_cast<short>(y), static_cast<short>(w),
                                     static_cast<short>(h));
            x_ = x;
            y_ = y;
        }
        static constexpr std::pair<int, int> map[] = {{1, BUTTON_LEFT}, {2, BUTTON_MIDDLE}, {4, BUTTON_RIGHT}};
        for (auto [bit, button] : map) {
            if ((buttons & bit) == (buttons_ & bit)) continue;
            int action = buttons & bit ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE;
            LiSendMouseButtonEvent(static_cast<char>(action), button);
        }
        buttons_ = buttons & 7;
    }

    /// Lets go of everything still held, so nothing stays pressed on the host.
    void release_input() {
        std::lock_guard g(input_lock_);
        auto keys = keyboard_.release_all();
        for (const KeyEvent& e : keys) LiSendKeyboardEvent2(e.key_code, e.action, e.modifiers, e.flags);
        static constexpr std::pair<int, int> map[] = {{1, BUTTON_LEFT}, {2, BUTTON_MIDDLE}, {4, BUTTON_RIGHT}};
        bool any = !keys.empty();
        for (auto [bit, button] : map) {
            if (buttons_ & bit) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, button);
                any = true;
            }
        }
        buttons_ = 0;
        // Give the input thread a moment to send them before it stops.
        if (any) std::this_thread::sleep_for(30ms);
    }

    casement::Host& host_;
    Options options_;
    DisplaySink sink_{host_};
    std::thread worker_;
    http::Canceller cancel_;
    std::mutex lock_;
    std::condition_variable cv_;
    bool stopping_ = false;
    bool starting_ = false;
    bool done_ = false;
    bool terminated_ = false;
    int termination_error_ = 0;
    int failed_stage_ = STAGE_NONE;
    int failed_error_ = 0;
    std::string app_;

    std::atomic<bool> streaming_{false};
    std::mutex input_lock_;
    Keyboard keyboard_;
    int x_ = -1;
    int y_ = -1;
    int buttons_ = 0;

    std::unique_ptr<VideoDecoder> video_;
    AudioPlayer audio_;
};

}  // namespace

std::unique_ptr<casement::Session> start_session(casement::Host& host, const Options& options) {
    return std::make_unique<MoonlightSession>(host, options);
}

}  // namespace moonlight
