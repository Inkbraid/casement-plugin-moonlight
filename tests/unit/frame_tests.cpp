// The way pictures reach Casement: one frame in flight, nothing written to
// the surface while it is, and a newer picture replacing a waiting one.
// Runs the plugin's DisplaySink on the real protocol layer, with a pipe as
// Casement and a file mapping as the surface.

#include "check.h"

#include <windows.h>

#include <array>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

#include "casement/protocol.h"
#include "moonlight/frames.h"
#include "moonlight/screen.h"

using namespace moonlight;
using casement::json;

namespace {

/// Casement's end of the plugin's output.
class Pipe {
public:
    Pipe() { CreatePipe(&read_, &write_, nullptr, 1 << 20); }
    ~Pipe() {
        CloseHandle(read_);
        CloseHandle(write_);
    }
    HANDLE writer() const { return write_; }

    /// The next message, or nothing if none is waiting.
    std::optional<json> next() {
        DWORD available = 0;
        if (!PeekNamedPipe(read_, nullptr, 0, nullptr, &available, nullptr) || available == 0) return std::nullopt;
        return casement::read_message(read_);
    }

private:
    HANDLE read_ = nullptr;
    HANDLE write_ = nullptr;
};

/// A surface as Casement makes one, given to the display as a `surface`
/// message.
class TestSurface {
public:
    TestSurface(casement::Host& host, std::uint32_t id, std::uint32_t width, std::uint32_t height)
        : stride_(width * 4 + 32) {
        std::size_t size = std::size_t{stride_} * height;
        mapping_ =
            CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(size), nullptr);
        view_ = static_cast<std::uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        HANDLE theirs = nullptr;
        DuplicateHandle(GetCurrentProcess(), mapping_, GetCurrentProcess(), &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS);
        host.display()->handle(json{{"type", "surface"},
                                    {"id", id},
                                    {"handle", reinterpret_cast<std::uintptr_t>(theirs)},
                                    {"width", width},
                                    {"height", height},
                                    {"stride", stride_},
                                    {"size", size}});
    }
    ~TestSurface() {
        UnmapViewOfFile(view_);
        CloseHandle(mapping_);
    }
    std::uint32_t pixel(std::uint32_t x, std::uint32_t y) const {
        std::uint32_t v;
        std::memcpy(&v, view_ + std::size_t{y} * stride_ + std::size_t{x} * 4, 4);
        return v;
    }

private:
    std::uint32_t stride_;
    HANDLE mapping_ = nullptr;
    std::uint8_t* view_ = nullptr;
};

/// A picture of one colour (BGRX as a little-endian number).
std::function<void(std::uint8_t*, std::size_t)> fill(std::uint32_t colour, int width, int height) {
    return [=](std::uint8_t* bgrx, std::size_t stride) {
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                std::memcpy(bgrx + std::size_t(y) * stride + std::size_t(x) * 4, &colour, 4);
    };
}

constexpr std::uint32_t red = 0xFFFF0000, green = 0xFF00FF00, blue = 0xFF0000FF;

}  // namespace

TEST(frames_one_in_flight_newest_wins) {
    Pipe pipe;
    casement::Host host(pipe.writer());
    DisplaySink sink(host);

    // Nothing to draw into before the size is known.
    sink.write_frame(fill(red, 64, 32));
    CHECK(!pipe.next());

    sink.set_size(64, 32);
    auto request = pipe.next();
    REQUIRE(request);
    CHECK_EQ((*request)["type"].get<std::string>(), std::string("surface_request"));
    CHECK_EQ((*request)["width"].get<int>(), 64);
    CHECK_EQ((*request)["height"].get<int>(), 32);

    // A picture before the surface arrives waits for it.
    sink.write_frame(fill(red, 64, 32));
    CHECK(!pipe.next());
    TestSurface surface(host, 7, 64, 32);
    auto frame = pipe.next();
    REQUIRE(frame);
    CHECK_EQ((*frame)["type"].get<std::string>(), std::string("frame"));
    CHECK_EQ((*frame)["surface"].get<int>(), 7);
    CHECK((*frame)["rects"].empty());  // the whole surface
    CHECK_EQ(surface.pixel(0, 0), red);
    CHECK_EQ(surface.pixel(63, 31), red);

    // Two more pictures while that frame is in flight: no frame is sent and
    // the surface isn't touched.
    sink.write_frame(fill(green, 64, 32));
    sink.write_frame(fill(blue, 64, 32));
    CHECK(!pipe.next());
    CHECK_EQ(surface.pixel(10, 10), red);

    // The acknowledgement brings the newest picture, once; green never
    // goes out.
    host.display()->handle(json{{"type", "frame_ack"}, {"surface", 7}});
    frame = pipe.next();
    REQUIRE(frame);
    CHECK_EQ((*frame)["type"].get<std::string>(), std::string("frame"));
    CHECK((*frame)["rects"].empty());
    CHECK_EQ(surface.pixel(10, 10), blue);
    CHECK(!pipe.next());

    // Nothing new: an acknowledgement sends nothing.
    host.display()->handle(json{{"type", "frame_ack"}, {"surface", 7}});
    CHECK(!pipe.next());

    // A free surface gets the next picture at once.
    sink.write_frame(fill(green, 64, 32));
    frame = pipe.next();
    REQUIRE(frame);
    CHECK_EQ(surface.pixel(10, 10), green);
    CHECK_EQ(sink.written(), std::uint64_t{4});

    // An acknowledgement for an old surface doesn't count.
    host.display()->handle(json{{"type", "frame_ack"}, {"surface", 6}});
    sink.write_frame(fill(red, 64, 32));
    CHECK(!pipe.next());
}

TEST(frames_new_size_new_surface) {
    Pipe pipe;
    casement::Host host(pipe.writer());
    DisplaySink sink(host);
    sink.set_size(screen_width, screen_height);
    REQUIRE(pipe.next());
    // The PIN screen first.
    sink.write_frame(fill(red, screen_width, screen_height));
    TestSurface pin_surface(host, 1, screen_width, screen_height);
    REQUIRE(pipe.next());
    CHECK_EQ(pin_surface.pixel(0, 0), red);
    // Then the stream's size: a new surface is asked for, and nothing is
    // drawn into the old one meanwhile, acknowledged or not.
    sink.set_size(1280, 720);
    auto request = pipe.next();
    REQUIRE(request);
    CHECK_EQ((*request)["width"].get<int>(), 1280);
    host.display()->handle(json{{"type", "frame_ack"}, {"surface", 1}});
    sink.write_frame(fill(green, 1280, 720));
    CHECK(!pipe.next());
    CHECK_EQ(pin_surface.pixel(0, 0), red);
    TestSurface stream(host, 2, 1280, 720);
    auto frame = pipe.next();
    REQUIRE(frame);
    CHECK_EQ((*frame)["surface"].get<int>(), 2);
    CHECK_EQ(stream.pixel(1279, 719), green);
    CHECK_EQ(sink.width(), 1280);
    CHECK_EQ(sink.height(), 720);
}

TEST(pin_screen_draws_the_pin) {
    std::size_t stride = std::size_t{screen_width} * 4;
    std::vector<std::uint8_t> buf(stride * screen_height, 0);
    Screen s = pin_screen("4821", "gaming-pc.lan");
    CHECK(s.text == L"Enter PIN 4821 on gaming-pc.lan (Sunshine web UI → PIN)");
    CHECK(s.big == L"4 8 2 1");
    draw_screen(s, buf.data(), screen_width, screen_height, stride);
    auto at = [&](int x, int y) {
        const std::uint8_t* p = buf.data() + std::size_t(y) * stride + std::size_t(x) * 4;
        return std::array<int, 4>{p[0], p[1], p[2], p[3]};
    };
    // The background in the corners, opaque.
    CHECK((at(2, 2) == std::array<int, 4>{34, 30, 28, 255}));
    CHECK((at(screen_width - 3, screen_height - 3) == std::array<int, 4>{34, 30, 28, 255}));
    // Bright pixels where the PIN is: the text was drawn.
    int bright = 0;
    for (int y = 150; y < 350; ++y)
        for (int x = 0; x < screen_width; ++x)
            if (at(x, y)[0] > 200 && at(x, y)[1] > 200 && at(x, y)[2] > 200) ++bright;
    CHECK(bright > 2000);
    int text = 0;
    for (int y = 360; y < 410; ++y)
        for (int x = 0; x < screen_width; ++x)
            if (at(x, y)[1] > 150) ++text;
    CHECK(text > 300);
}
