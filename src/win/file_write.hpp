#pragma once

#include <string>
#include <string_view>

namespace coax::win {

// Replaces the file at `path` with `bytes`, or leaves what was already there
// untouched. There is no third outcome — in particular there is no truncated
// file, which is what opening the destination directly would risk: creating a
// file for writing empties it first, so a write interrupted between those two
// moments destroys the old contents without having produced the new ones.
//
// Instead the bytes go to a temporary beside the destination and are renamed
// over it, which the filesystem does as one step. Beside it rather than in the
// system temp directory because a rename across volumes is a copy, and a copy
// is the thing being avoided.
//
// `what` names the file in the log for the failures worth reporting: "Settings",
// "Saved portal". Not the path, which holds the account name.
bool write_file_atomically(const std::wstring& path, std::string_view bytes,
                           std::string_view what);

}  // namespace coax::win
