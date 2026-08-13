#include "player/session_target_registry.hpp"

namespace coax::player {

std::uint64_t SessionTargetRegistry::begin_provider_session() {
    ++provider_session_;
    next_channel_session_ = 0;
    channel_sessions_.clear();
    return provider_session_;
}

SourceCorrelation SessionTargetRegistry::identify_channel(std::string_view channel_id) {
    if (const auto found = channel_sessions_.find(std::string{channel_id});
        found != channel_sessions_.end()) {
        return {
            .provider_session = provider_session_,
            .channel_session = found->second,
        };
    }
    return identify_fresh_channel(channel_id);
}

SourceCorrelation SessionTargetRegistry::identify_fresh_channel(std::string_view channel_id) {
    const auto channel_session = ++next_channel_session_;
    channel_sessions_.insert_or_assign(std::string{channel_id}, channel_session);
    return {
        .provider_session = provider_session_,
        .channel_session = channel_session,
    };
}

}  // namespace coax::player
