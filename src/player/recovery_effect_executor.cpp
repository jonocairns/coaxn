#include "player/recovery_effect_executor.hpp"

#include <type_traits>

namespace coax::player {

template<class>
inline constexpr bool kAlwaysFalse = false;

void execute_recovery_effect(const core::SupervisorEffect& effect,
                             const RecoveryExecutor* executor,
                             const SupervisorDispatch& dispatch,
                             const EffectRejectionSink& on_rejection) {
    std::optional<core::RecoveryTransport> transport;
    if (executor) {
        try {
            transport = std::visit([&](const auto& payload) {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, core::ReopenStream>) {
                    return executor->reopen_stream
                        ? executor->reopen_stream(effect.generation) : std::nullopt;
                } else if constexpr (std::is_same_v<T, core::ReloadHlsLive>) {
                    return executor->reload_hls_live
                        ? executor->reload_hls_live(effect.generation) : std::nullopt;
                } else if constexpr (std::is_same_v<T, core::ReopenProbed>) {
                    return executor->reopen_probed
                        ? executor->reopen_probed(effect.generation) : std::nullopt;
                } else if constexpr (std::is_same_v<T, core::RecreatePlayer>) {
                    return executor->recreate_player
                        ? executor->recreate_player(effect.generation) : std::nullopt;
                } else {
                    static_assert(kAlwaysFalse<T>, "SupervisorEffect handling must be exhaustive");
                }
            }, effect.payload);
        } catch (...) {
            transport.reset();
        }
    }
    if (!transport) {
        if (on_rejection) on_rejection(effect);
        dispatch(core::SourceFailed{effect.generation});
        return;
    }
    dispatch(core::StreamLoadIssued{effect.generation, *transport});
}

}  // namespace coax::player
