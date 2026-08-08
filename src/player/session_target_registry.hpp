#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "player/playback_observability.hpp"

namespace coax::player {

// Assigns log-safe, process-local identities to normalized provider channels.
// The registry never receives a stream URL or credentials. Re-selecting the
// same channel within one provider session returns the same identity, while a
// newly connected provider starts a distinct namespace.
class SessionTargetRegistry {
public:
    std::uint64_t begin_provider_session();
    SourceCorrelation identify_channel(std::string_view channel_id);
    [[nodiscard]] std::uint64_t provider_session() const { return provider_session_; }

private:
    std::uint64_t provider_session_ = 0;
    std::uint64_t next_channel_session_ = 0;
    std::unordered_map<std::string, std::uint64_t> channels_;
};

}  // namespace coax::player
