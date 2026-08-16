#pragma once

#include <string>

namespace coax::win {

// %LOCALAPPDATA%\Coax, created if it does not exist. Empty when the known
// folder cannot be resolved, which every caller has to treat as "there is
// nowhere to store this" rather than as a path relative to the working
// directory — the application is launched from wherever Explorer happens to be.
//
// Roaming would follow the user between machines, which is wrong for both of
// the things kept here: a DPAPI blob cannot be decrypted on another machine
// anyway, and a window preference belongs to the display it was chosen on.
std::wstring app_data_dir();

}  // namespace coax::win
