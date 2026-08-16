#pragma once

#include <string>
#include <string_view>

namespace coax::core {

// The loudest the player will go, as a percentage of unity — mpv takes volume
// past 100, and the headroom is deliberate. It lives here because a settings
// file is an untrusted input and the range has to be enforced where it is
// parsed; the slider that produces the value uses this same constant rather
// than a second copy of the number.
inline constexpr int kMaxVolume = 130;

// The preferences that survive a restart. Deliberately small: this is what the
// user has chosen, not what the application happens to be doing, so nothing
// that a session rebuilds for itself belongs here.
struct Settings {
    // Whether the window draws its own frame — no caption, an invisible strip
    // along the top edge to drag by, and the window commands in the right-click
    // menu. Off restores the ordinary Windows caption, which is the way back for
    // anyone who does not know the gesture and the fallback if the custom frame
    // misbehaves on a particular shell.
    bool minimal_mode = true;

    // Where the volume was left, as a percentage of unity. Restored rather than
    // reset because it is a property of the room the application is being
    // listened to in, and that does not change between sessions.
    int volume = 100;
};

// `key=value` lines. Not JSON, because the portable core has no link
// dependencies and is meant to keep it that way — and for a handful of flags a
// format someone can edit in Notepad is worth more than a parser.
std::string serialize(const Settings& settings);

// The inverse, and total: every input produces a Settings. A key that is absent,
// unrecognised, or carries a value that is not a boolean leaves its field at the
// default rather than failing the file, so a settings file written by a newer
// build — or damaged by half a disk write — still yields a usable application
// instead of none.
Settings parse(std::string_view text);

}  // namespace coax::core
