#include "moonlight/keymap.h"

#include <array>
#include <optional>
#include <vector>

namespace moonlight {

namespace {

// Windows virtual-key codes, by name, for the table below.
enum : std::uint8_t {
    VK_CANCEL_ = 0x03, VK_BACK_ = 0x08, VK_TAB_ = 0x09, VK_RETURN_ = 0x0D, VK_PAUSE_ = 0x13,
    VK_CAPITAL_ = 0x14, VK_CONVERT_ = 0x1C, VK_NONCONVERT_ = 0x1D, VK_ESCAPE_ = 0x1B, VK_SPACE_ = 0x20,
    VK_PRIOR_ = 0x21, VK_NEXT_ = 0x22, VK_END_ = 0x23, VK_HOME_ = 0x24, VK_LEFT_ = 0x25, VK_UP_ = 0x26,
    VK_RIGHT_ = 0x27, VK_DOWN_ = 0x28, VK_SNAPSHOT_ = 0x2C, VK_INSERT_ = 0x2D, VK_DELETE_ = 0x2E,
    VK_LWIN_ = 0x5B, VK_RWIN_ = 0x5C, VK_APPS_ = 0x5D, VK_SLEEP_ = 0x5F,
    VK_NUMPAD0_ = 0x60, VK_MULTIPLY_ = 0x6A, VK_ADD_ = 0x6B, VK_SUBTRACT_ = 0x6D, VK_DECIMAL_ = 0x6E,
    VK_DIVIDE_ = 0x6F, VK_F1_ = 0x70, VK_F11_ = 0x7A, VK_F13_ = 0x7C, VK_F24_ = 0x87, VK_NUMLOCK_ = 0x90,
    VK_SCROLL_ = 0x91, VK_LSHIFT_ = 0xA0, VK_RSHIFT_ = 0xA1, VK_LCONTROL_ = 0xA2, VK_RCONTROL_ = 0xA3,
    VK_LMENU_ = 0xA4, VK_RMENU_ = 0xA5, VK_BROWSER_BACK_ = 0xA6, VK_BROWSER_FORWARD_ = 0xA7,
    VK_BROWSER_REFRESH_ = 0xA8, VK_BROWSER_STOP_ = 0xA9, VK_BROWSER_SEARCH_ = 0xAA,
    VK_BROWSER_FAVORITES_ = 0xAB, VK_BROWSER_HOME_ = 0xAC, VK_VOLUME_MUTE_ = 0xAD, VK_VOLUME_DOWN_ = 0xAE,
    VK_VOLUME_UP_ = 0xAF, VK_MEDIA_NEXT_TRACK_ = 0xB0, VK_MEDIA_PREV_TRACK_ = 0xB1, VK_MEDIA_STOP_ = 0xB2,
    VK_MEDIA_PLAY_PAUSE_ = 0xB3, VK_LAUNCH_MAIL_ = 0xB4, VK_LAUNCH_MEDIA_SELECT_ = 0xB5,
    VK_LAUNCH_APP1_ = 0xB6, VK_LAUNCH_APP2_ = 0xB7, VK_OEM_1_ = 0xBA, VK_OEM_PLUS_ = 0xBB,
    VK_OEM_COMMA_ = 0xBC, VK_OEM_MINUS_ = 0xBD, VK_OEM_PERIOD_ = 0xBE, VK_OEM_2_ = 0xBF, VK_OEM_3_ = 0xC0,
    VK_OEM_4_ = 0xDB, VK_OEM_5_ = 0xDC, VK_OEM_6_ = 0xDD, VK_OEM_7_ = 0xDE, VK_OEM_102_ = 0xE2,
};

struct Table {
    std::array<KeyMapping, 0x80> plain{};
    std::array<KeyMapping, 0x80> e0{};

    constexpr Table() {
        auto set = [](std::array<KeyMapping, 0x80>& t, unsigned sc, std::uint8_t vk, bool ext = false,
                      bool raw = false) { t[sc] = KeyMapping{vk, ext, raw}; };
        // Main block, by position on a US keyboard.
        set(plain, 0x01, VK_ESCAPE_);
        const char digits[] = "1234567890";
        for (unsigned i = 0; i < 10; ++i) set(plain, 0x02 + i, static_cast<std::uint8_t>(digits[i]));
        set(plain, 0x0C, VK_OEM_MINUS_);
        set(plain, 0x0D, VK_OEM_PLUS_);
        set(plain, 0x0E, VK_BACK_);
        set(plain, 0x0F, VK_TAB_);
        const char row1[] = "QWERTYUIOP";
        for (unsigned i = 0; i < 10; ++i) set(plain, 0x10 + i, static_cast<std::uint8_t>(row1[i]));
        set(plain, 0x1A, VK_OEM_4_);
        set(plain, 0x1B, VK_OEM_6_);
        set(plain, 0x1C, VK_RETURN_);
        set(plain, 0x1D, VK_LCONTROL_);
        const char row2[] = "ASDFGHJKL";
        for (unsigned i = 0; i < 9; ++i) set(plain, 0x1E + i, static_cast<std::uint8_t>(row2[i]));
        set(plain, 0x27, VK_OEM_1_);
        set(plain, 0x28, VK_OEM_7_);
        set(plain, 0x29, VK_OEM_3_);
        set(plain, 0x2A, VK_LSHIFT_);
        set(plain, 0x2B, VK_OEM_5_);
        const char row3[] = "ZXCVBNM";
        for (unsigned i = 0; i < 7; ++i) set(plain, 0x2C + i, static_cast<std::uint8_t>(row3[i]));
        set(plain, 0x33, VK_OEM_COMMA_);
        set(plain, 0x34, VK_OEM_PERIOD_);
        set(plain, 0x35, VK_OEM_2_);
        set(plain, 0x36, VK_RSHIFT_);
        set(plain, 0x37, VK_MULTIPLY_);
        set(plain, 0x38, VK_LMENU_);
        set(plain, 0x39, VK_SPACE_);
        set(plain, 0x3A, VK_CAPITAL_);
        for (unsigned i = 0; i < 10; ++i) set(plain, 0x3B + i, static_cast<std::uint8_t>(VK_F1_ + i));
        set(plain, 0x45, VK_NUMLOCK_);
        set(plain, 0x46, VK_SCROLL_);
        // The numpad as numpad keys: the host's Num Lock state decides
        // whether they type digits or move the cursor.
        const unsigned numpad[] = {0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49};
        for (unsigned i = 0; i < 10; ++i) set(plain, numpad[i], static_cast<std::uint8_t>(VK_NUMPAD0_ + i));
        set(plain, 0x4A, VK_SUBTRACT_);
        set(plain, 0x4E, VK_ADD_);
        set(plain, 0x53, VK_DECIMAL_);
        set(plain, 0x54, VK_SNAPSHOT_);  // Alt+Print Screen
        set(plain, 0x56, VK_OEM_102_);   // the extra key of ISO keyboards
        set(plain, 0x57, VK_F11_);
        set(plain, 0x58, static_cast<std::uint8_t>(VK_F11_ + 1));
        for (unsigned i = 0; i < 11; ++i) set(plain, 0x64 + i, static_cast<std::uint8_t>(VK_F13_ + i));
        set(plain, 0x76, VK_F24_);
        // Japanese keys.
        set(plain, 0x73, VK_OEM_102_, false, true);  // Ro
        set(plain, 0x79, VK_CONVERT_);
        set(plain, 0x7B, VK_NONCONVERT_);
        set(plain, 0x7D, VK_OEM_5_, false, true);  // Yen

        // E0-prefixed keys.
        set(e0, 0x10, VK_MEDIA_PREV_TRACK_, true);
        set(e0, 0x19, VK_MEDIA_NEXT_TRACK_, true);
        set(e0, 0x1C, VK_RETURN_, true);  // Numpad Enter
        set(e0, 0x1D, VK_RCONTROL_, true);
        set(e0, 0x20, VK_VOLUME_MUTE_, true);
        set(e0, 0x21, VK_LAUNCH_APP2_, true);
        set(e0, 0x22, VK_MEDIA_PLAY_PAUSE_, true);
        set(e0, 0x24, VK_MEDIA_STOP_, true);
        set(e0, 0x2E, VK_VOLUME_DOWN_, true);
        set(e0, 0x30, VK_VOLUME_UP_, true);
        set(e0, 0x32, VK_BROWSER_HOME_, true);
        set(e0, 0x35, VK_DIVIDE_, true);
        set(e0, 0x37, VK_SNAPSHOT_, true);
        set(e0, 0x38, VK_RMENU_, true);
        set(e0, 0x46, VK_CANCEL_, true);  // Ctrl+Break
        set(e0, 0x47, VK_HOME_, true);
        set(e0, 0x48, VK_UP_, true);
        set(e0, 0x49, VK_PRIOR_, true);
        set(e0, 0x4B, VK_LEFT_, true);
        set(e0, 0x4D, VK_RIGHT_, true);
        set(e0, 0x4F, VK_END_, true);
        set(e0, 0x50, VK_DOWN_, true);
        set(e0, 0x51, VK_NEXT_, true);
        set(e0, 0x52, VK_INSERT_, true);
        set(e0, 0x53, VK_DELETE_, true);
        set(e0, 0x5B, VK_LWIN_, true);
        set(e0, 0x5C, VK_RWIN_, true);
        set(e0, 0x5D, VK_APPS_, true);
        set(e0, 0x5F, VK_SLEEP_, true);
        set(e0, 0x65, VK_BROWSER_SEARCH_, true);
        set(e0, 0x66, VK_BROWSER_FAVORITES_, true);
        set(e0, 0x67, VK_BROWSER_REFRESH_, true);
        set(e0, 0x68, VK_BROWSER_STOP_, true);
        set(e0, 0x69, VK_BROWSER_FORWARD_, true);
        set(e0, 0x6A, VK_BROWSER_BACK_, true);
        set(e0, 0x6B, VK_LAUNCH_APP1_, true);
        set(e0, 0x6C, VK_LAUNCH_MAIL_, true);
        set(e0, 0x6D, VK_LAUNCH_MEDIA_SELECT_, true);
    }
};

constexpr Table table;

bool is_modifier(std::uint32_t sc) {
    switch (sc) {
        case 0x2A: case 0x36: case 0x1D: case 0x11D: case 0x38: case 0x138: case 0x15B: case 0x15C:
            return true;
        default:
            return false;
    }
}

}  // namespace

std::optional<KeyMapping> map_scancode(std::uint32_t scancode) {
    if (scancode == pause_scancode) return KeyMapping{VK_PAUSE_, false, false};
    const KeyMapping* m = nullptr;
    if (scancode < 0x80) m = &table.plain[scancode];
    else if (scancode >= 0x100 && scancode < 0x180) m = &table.e0[scancode - 0x100];
    if (!m || m->vk == 0) return std::nullopt;
    return *m;
}

char Keyboard::modifiers() const {
    auto held = [&](std::uint32_t a, std::uint32_t b) { return down_.contains(a) || down_.contains(b); };
    int m = 0;
    if (held(0x2A, 0x36)) m |= modifier_shift;
    if (held(0x1D, 0x11D)) m |= modifier_ctrl;
    if (held(0x38, 0x138)) m |= modifier_alt;
    if (held(0x15B, 0x15C)) m |= modifier_meta;
    return static_cast<char>(m);
}

KeyEvent Keyboard::event(const KeyMapping& m, bool down) const {
    KeyEvent e;
    e.key_code = static_cast<short>(static_cast<std::uint16_t>(0x8000 | m.vk));
    e.action = down ? key_action_down : key_action_up;
    e.modifiers = static_cast<char>(modifiers() | (m.extended ? modifier_extended : 0));
    e.flags = m.non_normalized ? flag_non_normalized : 0;
    return e;
}

std::optional<KeyEvent> Keyboard::key(std::uint32_t scancode, bool down) {
    // Pause arrives as 0x21D, then 0x45; NumLock is 0x45 alone.
    if (scancode == 0x21D) {
        pause_prefix_ = true;
        return std::nullopt;
    }
    if (scancode == 0x45 && pause_prefix_) scancode = pause_scancode;
    pause_prefix_ = false;

    auto m = map_scancode(scancode);
    if (!m) return std::nullopt;
    if (down) {
        // Held keys repeat on the host; Casement's repeats would double them.
        if (!down_.insert(scancode).second) return std::nullopt;
    } else {
        down_.erase(scancode);
    }
    return event(*m, down);
}

std::vector<KeyEvent> Keyboard::release_all() {
    std::vector<KeyEvent> out;
    // Ordinary keys before modifiers, so a held Ctrl+C can't turn into a
    // plain "c" on the way up.
    for (bool modifiers_pass : {false, true}) {
        for (auto it = down_.begin(); it != down_.end();) {
            std::uint32_t sc = *it;
            if (is_modifier(sc) != modifiers_pass) {
                ++it;
                continue;
            }
            it = down_.erase(it);
            if (auto m = map_scancode(sc)) out.push_back(event(*m, false));
        }
    }
    pause_prefix_ = false;
    return out;
}

}  // namespace moonlight
