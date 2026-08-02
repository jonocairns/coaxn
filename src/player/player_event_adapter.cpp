#include "player/player_event_adapter.hpp"

#include <algorithm>

namespace coax::player {

void PlayerEventAdapter::track_load(std::uint64_t request_id, core::Generation generation) {
    pending_loads_.push_back({request_id, generation});
    load_requests_[request_id] = generation;
    backend_failure_reported_ = false;
}

void PlayerEventAdapter::track_property(std::uint64_t request_id, core::Generation generation,
                                        core::BufferPhase phase, BufferProperty property) {
    property_requests_[request_id] = {generation, phase, property};
}

void PlayerEventAdapter::command_result(std::uint64_t request_id, int error) {
    if (const auto load = load_requests_.find(request_id); load != load_requests_.end()) {
        events_.push_back({load->second, LoadCommandResult{request_id, error >= 0, error}});
        if (error < 0) remove_pending_load(request_id);
        load_requests_.erase(load);
        return;
    }
    if (const auto property = property_requests_.find(request_id);
        property != property_requests_.end()) {
        const auto request = property->second;
        events_.push_back({request.generation,
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
                intentional_stop(*active_entry_, active->second,
                                 IntentionalStopKind::Replaced);
            }
        }
        entries_[playlist_entry_id] = pending.generation;
    } else if (!entries_.contains(playlist_entry_id)) {
        return;
    }
    first_started_[playlist_entry_id] = false;
    active_entry_ = playlist_entry_id;
}

void PlayerEventAdapter::playback_restart(std::int64_t playlist_entry_id) {
    const auto entry = entries_.find(playlist_entry_id);
    if (entry == entries_.end() || first_started_[playlist_entry_id]) return;
    first_started_[playlist_entry_id] = true;
    events_.push_back({entry->second, FirstPlaybackStart{}});
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
        events_.push_back({stop->second.report_as, PlaybackStopped{stop->second.kind}});
        stop_intents_.erase(stop);
    } else {
        events_.push_back({generation, EndFileEvent{reason, error}});
    }
    entries_.erase(entry);
    first_started_.erase(playlist_entry_id);
    if (active_entry_ == playlist_entry_id) active_entry_.reset();
}

void PlayerEventAdapter::backend_failed(core::Generation generation, int error) {
    if (backend_failure_reported_) return;
    backend_failure_reported_ = true;
    events_.push_back({generation, BackendFailed{error}});
    // No command or playlist edge owned by the failed backend can complete
    // meaningfully. Keep the failure edge itself, but release every correlation.
    clear_correlations();
}

void PlayerEventAdapter::authentication_rejected(core::Generation generation) {
    events_.push_back({generation, PlayerAuthenticationRejected{}});
}

void PlayerEventAdapter::transport_failure(core::Generation generation,
                                           core::TransportFailureReason reason) {
    events_.push_back({generation, TransportFailureDetected{reason}});
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
                                  : std::optional<core::Generation>{found->second};
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
