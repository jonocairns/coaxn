#pragma once

#include <functional>
#include <optional>

#include "core/supervisor.hpp"

namespace coax::player {

struct RecoveryExecutor {
    std::function<std::optional<core::RecoveryTransport>(core::Generation,
                                                         core::LoadAttempt)> reopen_stream;
    std::function<std::optional<core::RecoveryTransport>(core::Generation,
                                                         core::LoadAttempt)> reload_hls_live;
    std::function<std::optional<core::RecoveryTransport>(core::Generation,
                                                         core::LoadAttempt)> reopen_probed;
    std::function<std::optional<core::RecoveryTransport>(core::Generation,
                                                         core::LoadAttempt)> recreate_player;
};

using SupervisorDispatch = std::function<void(const core::SupervisorEvent&)>;
using EffectRejectionSink = std::function<void(const core::SupervisorEffect&)>;

// Every path settles into a supervisor event. A missing target or exception is
// terminal rather than leaving recovery parked after its deadline was cleared.
void execute_recovery_effect(const core::SupervisorEffect& effect,
                             const RecoveryExecutor* executor,
                             const SupervisorDispatch& dispatch,
                             const EffectRejectionSink& on_rejection = {});

}  // namespace coax::player
