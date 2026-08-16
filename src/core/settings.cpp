#include "core/settings.hpp"

namespace coax::core {
namespace {

constexpr std::string_view kMinimalModeKey = "minimal_mode";
constexpr std::string_view kVolumeKey      = "volume";

// Both ends, because a file that has been through an editor picks up trailing
// whitespace and a file that has been through Windows line endings picks up a
// carriage return the split below leaves on the value.
std::string_view trim(std::string_view text) {
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r';
    };
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// Left alone rather than defaulted for anything that is not a boolean: the
// caller holds the field at whatever it already was, which is what makes a
// damaged line cost one setting instead of all of them.
void read_bool(std::string_view value, bool& out) {
    if (value == "true" || value == "1") {
        out = true;
    } else if (value == "false" || value == "0") {
        out = false;
    }
}

// Digits only, and clamped rather than rejected at the ends: a file naming a
// volume outside the range is stating an intention the application can honour
// approximately, where refusing it would silently restore something else. A
// sign, a decimal point or anything else leaves the field alone — that is not
// a number out of range, it is not a number.
void read_int(std::string_view value, int& out, int lowest, int highest) {
    if (value.empty()) {
        return;
    }

    int parsed = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') {
            return;
        }
        // Saturates instead of overflowing. The clamp below is what the caller
        // sees either way, so there is nothing to gain from the exact value of
        // a twenty-digit number.
        if (parsed <= highest) {
            parsed = parsed * 10 + (c - '0');
        }
    }

    out = parsed < lowest ? lowest : (parsed > highest ? highest : parsed);
}

}  // namespace

std::string serialize(const Settings& settings) {
    std::string text;
    text += kMinimalModeKey;
    text += '=';
    text += settings.minimal_mode ? "true" : "false";
    text += '\n';
    text += kVolumeKey;
    text += '=';
    text += std::to_string(settings.volume);
    text += '\n';
    return text;
}

Settings parse(std::string_view text) {
    Settings settings;

    while (!text.empty()) {
        const auto  break_at = text.find('\n');
        std::string_view line = text.substr(0, break_at);
        text = break_at == std::string_view::npos ? std::string_view{}
                                                  : text.substr(break_at + 1);

        line = trim(line);
        // A comment is not a setting, and neither is a blank line. Both are
        // skipped before the split so that neither can look like a key.
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const auto separator = line.find('=');
        if (separator == std::string_view::npos) {
            continue;
        }

        const std::string_view key   = trim(line.substr(0, separator));
        const std::string_view value = trim(line.substr(separator + 1));

        if (key == kMinimalModeKey) {
            read_bool(value, settings.minimal_mode);
        } else if (key == kVolumeKey) {
            read_int(value, settings.volume, 0, kMaxVolume);
        }
        // Anything else is a key this build does not know. Ignored rather than
        // treated as damage: an older binary reading a newer file is a downgrade
        // or a rollback, and it should keep the settings it does understand.
    }

    return settings;
}

}  // namespace coax::core
