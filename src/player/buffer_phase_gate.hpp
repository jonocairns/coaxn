#pragma once

#include "core/playback_types.hpp"

#include <optional>

namespace coax::player {

enum class BufferPhaseCommandState { Unissued, Pending, Applied, Failed };
enum class BufferPhaseProperty { CacheSeconds, ReadaheadSeconds };

inline const char* to_string(BufferPhaseCommandState value) {
    switch (value) {
        case BufferPhaseCommandState::Unissued: return "unissued";
        case BufferPhaseCommandState::Pending: return "pending";
        case BufferPhaseCommandState::Applied: return "applied";
        case BufferPhaseCommandState::Failed: return "failed";
    }
    return "unissued";
}

// Per-load command fence. A recovery reload keeps its generation but begins a
// new epoch, so both phases may be applied once again without accepting a stale
// generation or duplicating a phase within that load.
class BufferPhaseGate {
public:
    void begin_load(core::Generation generation) {
        generation_ = generation;
        has_load_ = true;
        zap_ = {};
        steady_ = {};
    }

    bool begin(core::Generation generation, core::BufferPhase phase) {
        if (!has_load_ || generation != generation_) return false;
        auto& record = phase == core::BufferPhase::Zap ? zap_ : steady_;
        if (record.state != BufferPhaseCommandState::Unissued) return false;
        record.state = BufferPhaseCommandState::Pending;
        return true;
    }

    [[nodiscard]] std::optional<BufferPhaseCommandState> settle(
        core::Generation generation, core::BufferPhase phase,
        BufferPhaseProperty property, bool accepted) {
        if (!has_load_ || generation != generation_) return std::nullopt;
        auto& record = phase == core::BufferPhase::Zap ? zap_ : steady_;
        if (record.state != BufferPhaseCommandState::Pending) return std::nullopt;
        auto& result = property == BufferPhaseProperty::CacheSeconds
            ? record.cache_accepted : record.readahead_accepted;
        if (result) return std::nullopt;
        result = accepted;
        if (!accepted) {
            record.state = BufferPhaseCommandState::Failed;
            return record.state;
        }
        if (!record.cache_accepted || !record.readahead_accepted) return std::nullopt;
        record.state = BufferPhaseCommandState::Applied;
        return record.state;
    }

    [[nodiscard]] BufferPhaseCommandState state(
        core::Generation generation, core::BufferPhase phase) const {
        if (!has_load_ || generation != generation_) return BufferPhaseCommandState::Unissued;
        return (phase == core::BufferPhase::Zap ? zap_ : steady_).state;
    }

private:
    struct PhaseRecord {
        BufferPhaseCommandState state = BufferPhaseCommandState::Unissued;
        std::optional<bool> cache_accepted;
        std::optional<bool> readahead_accepted;
    };

    core::Generation generation_;
    bool has_load_ = false;
    PhaseRecord zap_;
    PhaseRecord steady_;
};

}  // namespace coax::player
