#include "moonlight/frames.h"

#include <functional>

namespace moonlight {

void DisplaySink::set_size(int width, int height) {
    auto d = host_.display();
    d->set_size(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    width_ = width;
    height_ = height;
}

void DisplaySink::write_frame(const std::function<void(std::uint8_t* bgrx, std::size_t stride)>& draw) {
    auto d = host_.display();
    if (d->width() == 0) return;
    draw(d->buffer(), d->stride());
    ++written_;
    // The whole picture changed. flush() sends it now if Casement has
    // acknowledged the last frame; otherwise it waits in the back buffer,
    // where the next picture overwrites it, and the display sends whatever
    // is newest when the acknowledgement arrives.
    d->damage_all();
    d->flush();
}

}  // namespace moonlight
