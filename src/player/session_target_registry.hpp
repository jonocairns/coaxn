#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "player/playback_observability.hpp"

namespace coax::player {

// Assigns log-safe, process-local identities to live channels. Ordinary
// reselection retains the channel identity within one provider session so its
// loads can be grouped in diagnostics. Start explicitly refreshes that
// identity because returning to the live edge is a new playback session. Keys
// are provider channel IDs, never stream URLs or credentials.
class SessionTargetRegistry {
public:
    std::uint64_t begin_provider_session();
    SourceCorrelation identify_channel(std::string_view channel_id);
    SourceCorrelation identify_fresh_channel(std::string_view channel_id);
    [[nodiscard]] std::uint64_t provider_session() const { return provider_session_; }

private:
    std::uint64_t provider_session_ = 0;
    std::uint64_t next_channel_session_ = 0;
    std::unordered_map<std::string, std::uint64_t> channel_sessions_;
};

}  // namespace coax::player
