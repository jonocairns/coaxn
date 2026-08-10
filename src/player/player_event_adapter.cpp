#include "player/player_event_adapter.hpp"

#include <algorithm>

namespace coax::player {

void PlayerEventAdapter::track_load(std::uint64_t request_id, core::Generation generation,
                                    core::LoadAttempt load_attempt) {
    const LoadIdentity identity{generation, load_attempt};
    pending_loads_.push_back({request_id, identity});
    load_requests_[request_id] = identity;
    backend_failure_reported_ = false;
}

void PlayerEventAdapter::track_property(std::uint64_t request_id, core::Generation generation,
                                        core::BufferPhase phase, BufferProperty property) {
    property_requests_[request_id] = {generation, phase, property};
}

void PlayerEventAdapter::command_result(std::uint64_t request_id, int error) {
    if (const auto load = load_requests_.find(request_id); load != load_requests_.end()) {
        events_.push_back({load->second.generation, load->second.load_attempt,
                           LoadCommandResult{request_id, error >= 0, error}});
        if (error < 0) remove_pending_load(request_id);
        load_requests_.erase(load);
        return;
    }
    if (const auto property = property_requests_.find(request_id);
        property != property_requests_.end()) {
        const auto request = property->second;
        events_.push_back({request.generation, core::LoadAttempt{},
                           PropertyCommandResult{request_id, request.phase, request.property,
                                                 error >= 0, error}});
        property_requests_.erase(property);
    }
}

void PlayerEventAdapter::command_rejected_immediately(std::uint64_t request_id, int error) {
    command_result(request_id, error < 0 ? error : -1);
}

void PlayerEventAdapter::start_file(std::int64_t playlist_entry_id) {
    if (!pending_loads_.empty()) {
        const auto pending = pending_loads_.front();
        pending_loads_.pop_front();
        if (active_entry_) {
            const auto active = entries_.find(*active_entry_);
            if (active != entries_.end()) {
                intentional_stop(*active_entry_, active->second.generation,
                                 IntentionalStopKind::Replaced);
            }
        }
        entries_[playlist_entry_id] = pending.identity;
    } else if (!entries_.contains(playlist_entry_id)) {
        return;
    }
    first_started_[playlist_entry_id] = false;
    active_entry_ = playlist_entry_id;
}

void PlayerEventAdapter::playback_restart(std::int64_t playlist_entry_id) {
    const auto entry = entries_.find(playlist_entry_id);
    if (entry == entries_.end() || first_started_[playlist_entry_id]) return;

    // A queued replacement owns the next first-start edge, even when recovery
    // keeps the same generation. mpv can publish a late restart for the entry
    // being replaced after loadfile has been issued but before START_FILE for
    // the replacement. Letting that edge through would make the new load look
    // as though it had already produced a frame and turn its initial cache fill
    // into a learned rebuffer.
    if (!pending_loads_.empty()) return;

    first_started_[playlist_entry_id] = true;
    events_.push_back({entry->second.generation, entry->second.load_attempt,
                       FirstPlaybackStart{}});
}

void PlayerEventAdapter::intentional_stop(std::int64_t playlist_entry_id,
                                          core::Generation report_as,
                                          IntentionalStopKind kind) {
    stop_intents_[playlist_entry_id] = {report_as, kind};
}

void PlayerEventAdapter::end_file(std::int64_t playlist_entry_id,
                                  PlayerEndReason reason, int error,
                                  std::int64_t playlist_insert_id,
                                  int playlist_insert_num_entries) {
    const auto entry = entries_.find(playlist_entry_id);
    if (entry == entries_.end()) return;
    const auto generation = entry->second;
    if (reason == PlayerEndReason::Redirect && playlist_insert_num_entries > 0) {
        for (int offset = 0; offset < playlist_insert_num_entries; ++offset) {
            entries_[playlist_insert_id + offset] = generation;
        }
    }
    if (const auto stop = stop_intents_.find(playlist_entry_id);
        stop != stop_intents_.end()) {
        events_.push_back({stop->second.report_as, generation.load_attempt,
                           PlaybackStopped{stop->second.kind}});
        stop_intents_.erase(stop);
    } else {
        events_.push_back({generation.generation, generation.load_attempt,
                           EndFileEvent{reason, error}});
    }
    // Redirect insertion above may rehash entries_, invalidating `entry`.
    // Identity was copied before insertion, and key erase performs no invalid
    // iterator access.
    entries_.erase(playlist_entry_id);
    first_started_.erase(playlist_entry_id);
    if (active_entry_ == playlist_entry_id) active_entry_.reset();
}

void PlayerEventAdapter::backend_failed(core::Generation generation,
                                        core::LoadAttempt load_attempt, int error) {
    if (backend_failure_reported_) return;
    backend_failure_reported_ = true;
    events_.push_back({generation, load_attempt, BackendFailed{error}});
    // No command or playlist edge owned by the failed backend can complete
    // meaningfully. Keep the failure edge itself, but release every correlation.
    clear_correlations();
}

void PlayerEventAdapter::authentication_rejected(core::Generation generation,
                                                 core::LoadAttempt load_attempt) {
    events_.push_back({generation, load_attempt, PlayerAuthenticationRejected{}});
}

void PlayerEventAdapter::transport_failure(core::Generation generation,
                                           core::LoadAttempt load_attempt,
                                           core::TransportFailureReason reason) {
    events_.push_back({generation, load_attempt, TransportFailureDetected{reason}});
}

std::vector<PlayerEvent> PlayerEventAdapter::drain() {
    auto result = std::move(events_);
    events_.clear();
    return result;
}

std::optional<core::Generation> PlayerEventAdapter::active_generation() const {
    if (!active_entry_) return std::nullopt;
    const auto found = entries_.find(*active_entry_);
    return found == entries_.end() ? std::nullopt
                                  : std::optional<core::Generation>{found->second.generation};
}

std::optional<core::LoadAttempt> PlayerEventAdapter::active_load_attempt() const {
    if (!active_entry_) return std::nullopt;
    const auto found = entries_.find(*active_entry_);
    return found == entries_.end() ? std::nullopt
        : std::optional<core::LoadAttempt>{found->second.load_attempt};
}

void PlayerEventAdapter::dispose() {
    clear_correlations();
    events_.clear();
    backend_failure_reported_ = false;
}

void PlayerEventAdapter::clear_correlations() {
    pending_loads_.clear();
    load_requests_.clear();
    property_requests_.clear();
    entries_.clear();
    stop_intents_.clear();
    first_started_.clear();
    active_entry_.reset();
}

void PlayerEventAdapter::remove_pending_load(std::uint64_t request_id) {
    const auto found = std::find_if(pending_loads_.begin(), pending_loads_.end(),
                                    [request_id](const auto& pending) {
                                        return pending.request_id == request_id;
                                    });
    if (found != pending_loads_.end()) pending_loads_.erase(found);
}

}  // namespace coax::player
