#include "core/version.hpp"

namespace coax::core {
namespace {

// Guards against a hostile or malformed tag overflowing the accumulator; no
// real version component comes close to this many digits.
constexpr int kMaxComponentDigits = 6;

// Reads one run of digits, advancing `pos`. Returns false for an empty run or
// one long enough to be nonsense.
bool read_component(std::string_view text, std::size_t& pos, int& out) {
    const std::size_t start = pos;
    int value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        value = value * 10 + (text[pos] - '0');
        ++pos;
    }
    const std::size_t digits = pos - start;
    if (digits == 0 || digits > kMaxComponentDigits) {
        return false;
    }
    out = value;
    return true;
}

}  // namespace

std::optional<Version> parse_version(std::string_view text) {
    std::size_t pos = 0;
    if (pos < text.size() && (text[pos] == 'v' || text[pos] == 'V')) {
        ++pos;
    }

    Version version;
    if (!read_component(text, pos, version.major)) {
        return std::nullopt;
    }

    // Minor and patch are optional, but a dot must be followed by a component:
    // `1.` is malformed rather than shorthand for `1.0`.
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        if (!read_component(text, pos, version.minor)) {
            return std::nullopt;
        }
        if (pos < text.size() && text[pos] == '.') {
            ++pos;
            if (!read_component(text, pos, version.patch)) {
                return std::nullopt;
            }
        }
    }

    // Anything trailing means this is not a plain release tag.
    if (pos != text.size()) {
        return std::nullopt;
    }
    return version;
}

bool is_update_available(std::string_view available, std::string_view current) {
    const auto parsed_available = parse_version(available);
    const auto parsed_current   = parse_version(current);
    if (!parsed_available || !parsed_current) {
        return false;
    }
    return *parsed_available > *parsed_current;
}

}  // namespace coax::core
