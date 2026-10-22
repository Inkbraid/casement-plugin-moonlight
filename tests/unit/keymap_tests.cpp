// Set-1 scancodes to US-layout virtual keys, and the keyboard state.

#include "check.h"

#include <windows.h>

#include <cstdio>
#include <optional>
#include <vector>

#include "moonlight/keymap.h"

using namespace moonlight;

namespace {

void expect(std::uint32_t scancode, int vk, bool extended = false, bool raw = false) {
    auto m = map_scancode(scancode);
    if (!m) std::fprintf(stderr, "scancode %#x isn't mapped\n", scancode);
    REQUIRE(m);
    if (m->vk != vk)
        std::fprintf(stderr, "scancode %#x: vk %#x, expected %#x\n", scancode, unsigned{m->vk},
                     static_cast<unsigned>(vk));
    CHECK_EQ(int{m->vk}, vk);
    CHECK_EQ(m->extended, extended);
    CHECK_EQ(m->non_normalized, raw);
}

KeyEvent ev(int vk, char action, int modifiers, char flags = 0) {
    return KeyEvent{static_cast<short>(static_cast<std::uint16_t>(0x8000 | vk)), action, static_cast<char>(modifiers),
                    flags};
}

}  // namespace

TEST(keymap_letters_and_digits) {
    const char rows[][11] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    const unsigned starts[] = {0x10, 0x1E, 0x2C};
    for (int r = 0; r < 3; ++r)
        for (unsigned i = 0; rows[r][i]; ++i) expect(starts[r] + i, rows[r][i]);
    for (unsigned i = 0; i < 9; ++i) expect(0x02 + i, '1' + static_cast<int>(i));
    expect(0x0B, '0');
}

TEST(keymap_punctuation_us_positions) {
    expect(0x0C, 0xBD);  // -
    expect(0x0D, 0xBB);  // =
    expect(0x1A, 0xDB);  // [
    expect(0x1B, 0xDD);  // ]
    expect(0x27, 0xBA);  // ;
    expect(0x28, 0xDE);  // '
    expect(0x29, 0xC0);  // `
    expect(0x2B, 0xDC);  // backslash
    expect(0x33, 0xBC);  // ,
    expect(0x34, 0xBE);  // .
    expect(0x35, 0xBF);  // /
    expect(0x56, 0xE2);  // ISO key between Shift and Z
}

TEST(keymap_control_keys) {
    expect(0x01, 0x1B);
    expect(0x0E, 0x08);
    expect(0x0F, 0x09);
    expect(0x1C, 0x0D);
    expect(0x11C, 0x0D, true);  // Numpad Enter
    expect(0x39, 0x20);
    expect(0x3A, 0x14);
    expect(0x45, 0x90);
    expect(0x46, 0x91);
    expect(0x137, 0x2C, true);
    expect(0x54, 0x2C);
    expect(0x146, 0x03, true);
}

TEST(keymap_modifiers) {
    expect(0x2A, 0xA0);
    expect(0x36, 0xA1);
    expect(0x1D, 0xA2);
    expect(0x11D, 0xA3, true);
    expect(0x38, 0xA4);
    expect(0x138, 0xA5, true);
    expect(0x15B, 0x5B, true);
    expect(0x15C, 0x5C, true);
    expect(0x15D, 0x5D, true);
}

TEST(keymap_navigation_and_numpad) {
    expect(0x147, 0x24, true);
    expect(0x148, 0x26, true);
    expect(0x149, 0x21, true);
    expect(0x14B, 0x25, true);
    expect(0x14D, 0x27, true);
    expect(0x14F, 0x23, true);
    expect(0x150, 0x28, true);
    expect(0x151, 0x22, true);
    expect(0x152, 0x2D, true);
    expect(0x153, 0x2E, true);
    // The numpad stays the numpad; the host's Num Lock decides.
    expect(0x52, 0x60);
    expect(0x4F, 0x61);
    expect(0x50, 0x62);
    expect(0x51, 0x63);
    expect(0x4B, 0x64);
    expect(0x4C, 0x65);
    expect(0x4D, 0x66);
    expect(0x47, 0x67);
    expect(0x48, 0x68);
    expect(0x49, 0x69);
    expect(0x37, 0x6A);
    expect(0x4E, 0x6B);
    expect(0x4A, 0x6D);
    expect(0x53, 0x6E);
    expect(0x135, 0x6F, true);
}

TEST(keymap_function_media_and_japanese_keys) {
    for (unsigned i = 0; i < 10; ++i) expect(0x3B + i, 0x70 + static_cast<int>(i));
    expect(0x57, 0x7A);
    expect(0x58, 0x7B);
    expect(0x64, 0x7C);
    expect(0x6E, 0x86);
    expect(0x76, 0x87);
    expect(0x120, 0xAD, true);
    expect(0x12E, 0xAE, true);
    expect(0x130, 0xAF, true);
    expect(0x122, 0xB3, true);
    expect(0x110, 0xB1, true);
    expect(0x119, 0xB0, true);
    expect(0x16A, 0xA6, true);
    expect(0x73, 0xE2, false, true);  // Ro
    expect(0x7D, 0xDC, false, true);  // Yen
    expect(0x79, 0x1C);
    expect(0x7B, 0x1D);
    CHECK(!map_scancode(0x00));
    CHECK(!map_scancode(0x5A));
    CHECK(!map_scancode(0x17F));
    CHECK(!map_scancode(0x300));
}

// The table against Windows' own US layout, for every key where the
// GameStream mapping is the layout's (not the numpad, which stays numpad
// keys, nor keys the US layout doesn't have).
TEST(keymap_agrees_with_windows_us_layout) {
    HKL us = LoadKeyboardLayoutW(L"00000409", KLF_NOTELLSHELL);
    if (!us) {
        std::fprintf(stderr, "no US keyboard layout here; skipped\n");
        return;
    }
    std::vector<std::uint32_t> codes;
    for (std::uint32_t sc = 0x01; sc <= 0x0E; ++sc) codes.push_back(sc);
    for (std::uint32_t sc = 0x0F; sc <= 0x36; ++sc) codes.push_back(sc);
    for (std::uint32_t sc : {0x38u, 0x39u, 0x3Au, 0x3Bu, 0x3Cu, 0x3Du, 0x3Eu, 0x3Fu, 0x40u, 0x41u, 0x42u, 0x43u, 0x44u,
                             0x46u, 0x56u, 0x57u, 0x58u, 0x11Du, 0x138u, 0x147u, 0x148u, 0x149u, 0x14Bu, 0x14Du, 0x14Fu,
                             0x150u, 0x151u, 0x152u, 0x153u, 0x135u, 0x11Cu})
        codes.push_back(sc);
    int differ = 0;
    for (std::uint32_t sc : codes) {
        UINT code = sc >= 0x100 ? (0xE000 | (sc & 0xFF)) : sc;
        UINT vk = MapVirtualKeyExW(code, MAPVK_VSC_TO_VK_EX, us);
        auto m = map_scancode(sc);
        REQUIRE(m);
        if (vk != m->vk) {
            ++differ;
            std::fprintf(stderr, "scancode %#x: table %#x, Windows %#x\n", sc, unsigned{m->vk}, vk);
        }
    }
    CHECK_EQ(differ, 0);
}

TEST(keyboard_shift_a) {
    Keyboard k;
    CHECK(k.key(0x2A, true) == std::optional(ev(0xA0, key_action_down, modifier_shift)));
    CHECK(k.key(0x1E, true) == std::optional(ev('A', key_action_down, modifier_shift)));
    CHECK(k.key(0x1E, false) == std::optional(ev('A', key_action_up, modifier_shift)));
    CHECK(k.key(0x2A, false) == std::optional(ev(0xA0, key_action_up, 0)));
    CHECK_EQ(k.modifiers(), char{0});
}

TEST(keyboard_extended_and_modifier_bits) {
    Keyboard k;
    CHECK(k.key(0x11D, true) == std::optional(ev(0xA3, key_action_down, modifier_ctrl | modifier_extended)));
    CHECK(k.key(0x138, true) ==
          std::optional(ev(0xA5, key_action_down, modifier_ctrl | modifier_alt | modifier_extended)));
    CHECK(k.key(0x15B, true) ==
          std::optional(ev(0x5B, key_action_down, modifier_ctrl | modifier_alt | modifier_meta | modifier_extended)));
    CHECK(k.key(0x148, true) ==
          std::optional(ev(0x26, key_action_down, modifier_ctrl | modifier_alt | modifier_meta | modifier_extended)));
    CHECK(k.key(0x73, true) ==
          std::optional(ev(0xE2, key_action_down, modifier_ctrl | modifier_alt | modifier_meta, flag_non_normalized)));
}

TEST(keyboard_drops_repeats) {
    Keyboard k;
    CHECK(k.key(0x1E, true));
    CHECK(!k.key(0x1E, true));
    CHECK(!k.key(0x1E, true));
    CHECK(k.key(0x1E, false));
    // A key-up for a key never seen down still goes out.
    CHECK(k.key(0x1F, false));
    CHECK(!k.key(0x5A, true));
}

TEST(keyboard_pause_and_numlock) {
    Keyboard k;
    CHECK(!k.key(0x21D, true));
    CHECK(k.key(0x45, true) == std::optional(ev(0x13, key_action_down, 0)));
    CHECK(!k.key(0x21D, false));
    CHECK(k.key(0x45, false) == std::optional(ev(0x13, key_action_up, 0)));
    CHECK(k.key(0x45, true) == std::optional(ev(0x90, key_action_down, 0)));
    CHECK(k.key(0x45, false) == std::optional(ev(0x90, key_action_up, 0)));
}

TEST(keyboard_release_all) {
    Keyboard k;
    k.key(0x1D, true);
    k.key(0x38, true);
    k.key(0x153, true);
    auto ups = k.release_all();
    REQUIRE(ups.size() == 3);
    // Delete first, still with Ctrl and Alt held; then the modifiers.
    CHECK(ups[0] == ev(0x2E, key_action_up, modifier_ctrl | modifier_alt | modifier_extended));
    for (const auto& e : ups) CHECK_EQ(e.action, key_action_up);
    CHECK_EQ(k.modifiers(), char{0});
    CHECK(k.release_all().empty());
}
