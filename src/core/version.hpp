#pragma once

#include <compare>
#include <optional>
#include <string_view>

namespace coax::core {

// A release version, ordered the way semantic versioning orders them.
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;

    // Member order is major, minor, patch, so the defaulted comparison is
    // already the ordering semantic versioning specifies.
    friend auto operator<=>(const Version&, const Version&) = default;
    friend bool operator==(const Version&, const Version&)  = default;
};

// Parses `1.2.3`, with an optional leading `v` and with the minor and patch
// components optional. Returns nothing for anything else, pre-release tags
// (`v1.2.3-rc1`) included: a build should never invite someone onto a
// pre-release it cannot recognise, and silence is the safe answer.
std::optional<Version> parse_version(std::string_view text);

// Whether `available` is a release worth telling the user about. False when
// either side fails to parse, so an unreadable tag never produces a prompt.
bool is_update_available(std::string_view available, std::string_view current);

}  // namespace coax::core
