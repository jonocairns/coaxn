#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>

namespace coax::core {

using Duration = std::chrono::duration<double>;
using TimePoint = std::chrono::time_point<std::chrono::steady_clock, Duration>;

constexpr Duration seconds(double value) { return Duration{value}; }
constexpr Duration milliseconds(double value) {
    return std::chrono::duration_cast<Duration>(std::chrono::duration<double, std::milli>{value});
}

class Generation {
public:
    constexpr Generation() = default;
    explicit constexpr Generation(std::uint64_t value) : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const { return value_; }
    constexpr auto operator<=>(const Generation&) const = default;

private:
    std::uint64_t value_ = 0;
};

enum class GenerationDecision { Stale, Current, Future };

constexpr GenerationDecision decide_generation(Generation latest, Generation candidate) {
    if (candidate == latest) return GenerationDecision::Current;
    return candidate < latest ? GenerationDecision::Stale : GenerationDecision::Future;
}

constexpr bool should_apply_generation(Generation displayed, Generation candidate) {
    return candidate >= displayed;
}

enum class BufferPhase { Zap, Steady };
enum class RecoveryTransport { MpegTs, Hls };

struct BufferPhaseTargets {
    double cache_seconds;
    double readahead_seconds;
};

}  // namespace coax::core
