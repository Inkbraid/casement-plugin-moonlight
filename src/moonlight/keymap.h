// Casement's keys (PC set-1 scancodes, 0x100 added for E0-prefixed keys)
// as GameStream keyboard events: Windows virtual-key codes as they are on a
// US keyboard. The host turns them back into scancodes, so mapping by
// position gives the right physical key whatever the layouts on either
// side; the host's own layout then decides which character it types.

#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <vector>

namespace moonlight {

struct KeyMapping {
    std::uint8_t vk = 0;
    /// The key carries the E0 prefix (MODIFIER_EXTENDED): Numpad Enter
    /// versus Enter, the arrows versus the numpad.
    bool extended = false;
    /// Keys a US keyboard doesn't have (Japanese Ro and Yen): the host
    /// takes the virtual key as it is (SS_KBE_FLAG_NON_NORMALIZED).
    bool non_normalized = false;
};

/// The mapping for a scancode, or nothing for keys GameStream can't send.
std::optional<KeyMapping> map_scancode(std::uint32_t scancode);

// Values of Limelight.h, repeated so this header needs no library.
inline constexpr char key_action_down = 0x03;
inline constexpr char key_action_up = 0x04;
inline constexpr char modifier_shift = 0x01;
inline constexpr char modifier_ctrl = 0x02;
inline constexpr char modifier_alt = 0x04;
inline constexpr char modifier_meta = 0x08;
inline constexpr char modifier_extended = 0x10;
inline constexpr char flag_non_normalized = 0x01;

/// One LiSendKeyboardEvent2 call.
struct KeyEvent {
    short key_code = 0;  // 0x8000 | virtual key, as Moonlight sends it
    char action = key_action_down;
    char modifiers = 0;
    char flags = 0;

    bool operator==(const KeyEvent&) const = default;
};

/// Turns Casement's key messages into keyboard events: tracks which keys
/// are down for the modifier byte, drops repeated key-downs (the host
/// repeats held keys itself) and joins Pause's two-part sequence.
class Keyboard {
public:
    std::optional<KeyEvent> key(std::uint32_t scancode, bool down);

    /// Key-ups for every key still down, so nothing stays held on the host.
    std::vector<KeyEvent> release_all();

    char modifiers() const;

private:
    KeyEvent event(const KeyMapping& m, bool down) const;

    std::set<std::uint32_t> down_;
    bool pause_prefix_ = false;
};

/// Casement sends Pause as 0x21D followed by 0x45; the plugin keeps track
/// of it under this code.
inline constexpr std::uint32_t pause_scancode = 0x245;

}  // namespace moonlight
