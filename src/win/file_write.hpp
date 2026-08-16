#pragma once

#include <string>
#include <string_view>

namespace coax::win {

// Replaces the file at `path` with `bytes`, or leaves what was there untouched.
// There is no third outcome, and in particular no truncated file: opening the
// destination directly would empty it first, so an interrupted write destroys
// the old contents without having produced the new ones. The bytes go to a
// temporary beside the destination — beside, because a rename across volumes is
// a copy — and are renamed over it in one step.
//
// `what` names the file in the log: "Settings", "Saved portal". Not the path,
// which holds the account name.
bool write_file_atomically(const std::wstring& path, std::string_view bytes,
                           std::string_view what);

}  // namespace coax::win
