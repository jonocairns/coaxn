#pragma once

#include "core/settings.hpp"

namespace coax::win {

// The settings file, at %LOCALAPPDATA%\Coax\settings.txt. Plaintext, unlike the
// portal next to it: none of this is secret, and a preference someone can fix
// in Notepad is a preference that cannot lock them out of their own window.
class SettingsStore {
public:
    // Defaults whenever there is no file, it cannot be read, or it is damaged.
    // There is no failure to report: an absent settings file is the first run,
    // and the application has to start either way.
    static core::Settings load();

    // Returns false if the file could not be written. Callers may ignore it —
    // failing to persist a preference is not worth interrupting anyone over,
    // and it is logged here.
    static bool save(const core::Settings& settings);
};

}  // namespace coax::win
