#include "player/session_target_registry.hpp"

namespace coax::player {

std::uint64_t SessionTargetRegistry::begin_provider_session() {
    ++provider_session_;
    next_channel_session_ = 0;
    channels_.clear();
    return provider_session_;
}

SourceCorrelation SessionTargetRegistry::identify_channel(std::string_view channel_id) {
    auto [entry, inserted] = channels_.try_emplace(std::string(channel_id), 0);
    if (inserted) entry->second = ++next_channel_session_;
    return {
        .provider_session = provider_session_,
        .channel_session = entry->second,
    };
}

}  // namespace coax::player
