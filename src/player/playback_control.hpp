#pragma once

#include <string>

namespace coax::player {

// A source earns position-preserving controls only after the player has
// established that it can actually resume inside a seekable live window.
// Current MPEG-TS playback always uses RestartAtLiveEdge.
enum class PlaybackControlCapability { RestartAtLiveEdge, ResumeFromPosition };

// User intent is deliberately independent of the observed player lifecycle.
// StoppedByUser is the current TS state; SuspendedByUser reserves the distinct
// position-preserving intent for a future capable source.
enum class PlaybackIntent { Running, StoppedByUser, SuspendedByUser };
enum class PlaybackControl { Start, Stop, Pause, Resume };
enum class LiveStartDecision { NoSelection, StartFresh, RetainedChannelMissing };

[[nodiscard]] constexpr PlaybackControl playback_control(
    PlaybackControlCapability capability, PlaybackIntent intent) {
    if (capability == PlaybackControlCapability::ResumeFromPosition) {
        return intent == PlaybackIntent::SuspendedByUser
            ? PlaybackControl::Resume : PlaybackControl::Pause;
    }
    return intent == PlaybackIntent::StoppedByUser
        ? PlaybackControl::Start : PlaybackControl::Stop;
}

[[nodiscard]] constexpr const char* control_label(PlaybackControl control) {
    switch (control) {
        case PlaybackControl::Start: return "Start";
        case PlaybackControl::Stop: return "Stop";
        case PlaybackControl::Pause: return "Pause";
        case PlaybackControl::Resume: return "Resume";
    }
    return "Start";
}

[[nodiscard]] constexpr bool position_preserving_pause_requested(
    PlaybackControlCapability capability, PlaybackIntent intent) {
    return capability == PlaybackControlCapability::ResumeFromPosition &&
           intent == PlaybackIntent::SuspendedByUser;
}

// App resolves the retained id before changing intent or minting a generation.
// Keeping this decision explicit makes a missing catalog entry different from
// both "nothing selected" and a fresh live request.
[[nodiscard]] inline LiveStartDecision prepare_live_start(
    std::string& retained_channel_id, PlaybackIntent& intent,
    bool retained_channel_exists) {
    if (retained_channel_id.empty()) return LiveStartDecision::NoSelection;
    if (!retained_channel_exists) {
        retained_channel_id.clear();
        return LiveStartDecision::RetainedChannelMissing;
    }
    intent = PlaybackIntent::Running;
    return LiveStartDecision::StartFresh;
}

}  // namespace coax::player
