// Text drawn into the surface while there is no stream to show: protocol
// v1 has no other way to tell the user the pairing PIN (proposal P2 would
// add prompts).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace moonlight {

inline constexpr int screen_width = 960;
inline constexpr int screen_height = 540;

struct Screen {
    std::wstring title;  // a line at the top
    std::wstring big;    // large, in the middle: the PIN
    std::wstring text;   // below it
    std::wstring note;   // small, at the bottom
};

/// Draws the screen into a BGRX buffer with GDI.
void draw_screen(const Screen& screen, std::uint8_t* bgrx, int width, int height, std::size_t stride);

/// The pairing screen: "Enter PIN 4821 on gaming-pc.lan (Sunshine web UI → PIN)".
Screen pin_screen(const std::string& pin, const std::string& host);

}  // namespace moonlight
