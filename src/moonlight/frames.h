// Pictures on their way to Casement. The decoder writes each picture into
// the display's back buffer; the display copies it into the surface and
// sends `frame` when no frame is in flight. A picture that arrives while
// one is in flight replaces the waiting one, so Casement always gets the
// newest and nothing queues up.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "casement/plugin.h"

namespace moonlight {

/// Where pictures go.
class FrameSink {
public:
    virtual ~FrameSink() = default;
    /// The picture's size changed (or is first known).
    virtual void set_size(int width, int height) = 0;
    /// Calls `draw` with the buffer for a whole picture (BGRX, `stride`
    /// bytes per row, of the size last set), then shows it. Nothing is
    /// drawn before the size is known.
    virtual void write_frame(const std::function<void(std::uint8_t* bgrx, std::size_t stride)>& draw) = 0;
};

/// The FrameSink for Casement's display.
class DisplaySink final : public FrameSink {
public:
    explicit DisplaySink(casement::Host& host) : host_(host) {}

    void set_size(int width, int height) override;
    void write_frame(const std::function<void(std::uint8_t* bgrx, std::size_t stride)>& draw) override;

    int width() const { return width_; }
    int height() const { return height_; }
    std::uint64_t written() const { return written_; }

private:
    casement::Host& host_;
    std::atomic<int> width_{0};
    std::atomic<int> height_{0};
    std::atomic<std::uint64_t> written_{0};
};

}  // namespace moonlight
