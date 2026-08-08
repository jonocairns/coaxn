#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

#include "player/buffer_phase_gate.hpp"
#include "player/load_diagnostics.hpp"
#include "player/player_event_adapter.hpp"
#include "player/playback_observability.hpp"
#include "player/recovery_effect_executor.hpp"
#include "player/session_target_registry.hpp"
#include "player/transport_log_classifier.hpp"

using namespace coax;

TEST_CASE("adapter drains every edge once in mpv order with issue-time generations") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(10, core::Generation{1});
    adapter.command_result(10, 0);
    adapter.start_file(100);
    adapter.playback_restart(100);
    adapter.track_load(11, core::Generation{2});
    adapter.command_result(11, 0);
    adapter.start_file(101);
    adapter.end_file(100, player::PlayerEndReason::Stop, 0);
    adapter.playback_restart(101);

    const auto events = adapter.drain();
    REQUIRE(events.size() == 5);
    CHECK(events[0].generation == core::Generation{1});
    CHECK(std::holds_alternative<player::LoadCommandResult>(events[0].payload));
    CHECK(events[1].generation == core::Generation{1});
    CHECK(std::holds_alternative<player::FirstPlaybackStart>(events[1].payload));
    CHECK(events[2].generation == core::Generation{2});
    CHECK(std::holds_alternative<player::LoadCommandResult>(events[2].payload));
    CHECK(events[3].generation == core::Generation{1});
    CHECK(std::get<player::PlaybackStopped>(events[3].payload).kind ==
          player::IntentionalStopKind::Replaced);
    CHECK(events[4].generation == core::Generation{2});
    CHECK(std::holds_alternative<player::FirstPlaybackStart>(events[4].payload));
    CHECK(adapter.drain().empty());
}

TEST_CASE("a superseded load cannot publish a late first playback start") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(10, core::Generation{4});
    adapter.command_result(10, 0);
    adapter.start_file(100);
    REQUIRE(adapter.drain().size() == 1);

    // Recovery retains the generation. Until its START_FILE arrives, the old
    // entry is still active and may publish a delayed restart; that edge belongs
    // to the load being replaced and must not arm the new load's rebuffer gate.
    adapter.track_load(11, core::Generation{4});
    adapter.command_result(11, 0);
    REQUIRE(adapter.drain().size() == 1);
    adapter.playback_restart(100);
    CHECK(adapter.drain().empty());

    adapter.start_file(101);
    adapter.end_file(100, player::PlayerEndReason::Stop, 0);
    adapter.playback_restart(101);
    const auto events = adapter.drain();
    REQUIRE(events.size() == 2);
    CHECK(std::get<player::PlaybackStopped>(events[0].payload).kind ==
          player::IntentionalStopKind::Replaced);
    CHECK(std::holds_alternative<player::FirstPlaybackStart>(events[1].payload));
    CHECK(events[1].generation == core::Generation{4});
}

TEST_CASE("structured end reason stays attached to its load generation") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(10, core::Generation{7});
    adapter.command_result(10, 0);
    adapter.start_file(70);
    adapter.end_file(70, player::PlayerEndReason::Error, -13);
    const auto events = adapter.drain();
    REQUIRE(events.size() == 2);
    const auto& ended = std::get<player::EndFileEvent>(events[1].payload);
    CHECK(events[1].generation == core::Generation{7});
    CHECK(ended.reason == player::PlayerEndReason::Error);
    CHECK(ended.error == -13);
}

TEST_CASE("HLS redirect transfers generation ownership to inserted entries") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(12, core::Generation{8});
    adapter.command_result(12, 0);
    adapter.start_file(80);
    adapter.end_file(80, player::PlayerEndReason::Redirect, 0, 81, 2);
    adapter.start_file(81);
    adapter.playback_restart(81);
    adapter.end_file(81, player::PlayerEndReason::Eof, 0);
    const auto events = adapter.drain();
    REQUIRE(events.size() == 4);
    for (const auto& event : events) CHECK(event.generation == core::Generation{8});
    CHECK(std::get<player::EndFileEvent>(events[1].payload).reason ==
          player::PlayerEndReason::Redirect);
    CHECK(std::holds_alternative<player::FirstPlaybackStart>(events[2].payload));
    CHECK(std::get<player::EndFileEvent>(events[3].payload).reason ==
          player::PlayerEndReason::Eof);
}

TEST_CASE("load and buffer property command rejection are observable") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(10, core::Generation{3});
    adapter.command_rejected_immediately(10, -4);
    adapter.track_property(20, core::Generation{3}, core::BufferPhase::Steady,
                           player::BufferProperty::CacheSeconds);
    adapter.command_result(20, -9);
    const auto events = adapter.drain();
    REQUIRE(events.size() == 2);
    const auto& load = std::get<player::LoadCommandResult>(events[0].payload);
    CHECK_FALSE(load.accepted);
    CHECK(load.error == -4);
    const auto& property = std::get<player::PropertyCommandResult>(events[1].payload);
    CHECK_FALSE(property.accepted);
    CHECK(property.phase == core::BufferPhase::Steady);
    CHECK(property.property == player::BufferProperty::CacheSeconds);
}

TEST_CASE("one backend failure produces one edge until a new load is issued") {
    player::PlayerEventAdapter adapter;
    adapter.backend_failed(core::Generation{2}, -1);
    adapter.backend_failed(core::Generation{2}, -2);
    auto events = adapter.drain();
    REQUIRE(events.size() == 1);
    CHECK(std::holds_alternative<player::BackendFailed>(events[0].payload));
    adapter.track_load(30, core::Generation{2});
    adapter.backend_failed(core::Generation{2}, -3);
    events = adapter.drain();
    REQUIRE(events.size() == 1);
    CHECK(std::get<player::BackendFailed>(events[0].payload).error == -3);
}

TEST_CASE("backend failure and disposal release stale correlations") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(10, core::Generation{2});
    adapter.track_property(20, core::Generation{2}, core::BufferPhase::Zap,
                           player::BufferProperty::CacheSeconds);
    adapter.backend_failed(core::Generation{2}, -7);
    adapter.command_result(10, 0);
    adapter.command_result(20, 0);
    adapter.start_file(200);
    auto events = adapter.drain();
    REQUIRE(events.size() == 1);
    CHECK(std::get<player::BackendFailed>(events[0].payload).error == -7);
    CHECK_FALSE(adapter.active_generation());

    adapter.track_load(30, core::Generation{3});
    adapter.command_result(30, 0);
    adapter.dispose();
    CHECK(adapter.drain().empty());
    adapter.start_file(300);
    CHECK_FALSE(adapter.active_generation());
}

TEST_CASE("explicit and replacement stops are classified separately") {
    player::PlayerEventAdapter adapter;
    adapter.track_load(1, core::Generation{4}); adapter.command_result(1, 0);
    adapter.start_file(40);
    adapter.intentional_stop(40, core::Generation{5},
                             player::IntentionalStopKind::Requested);
    adapter.end_file(40, player::PlayerEndReason::Stop, 0);
    const auto events = adapter.drain();
    REQUIRE(events.size() == 2);
    CHECK(events[1].generation == core::Generation{5});
    CHECK(std::get<player::PlaybackStopped>(events[1].payload).kind ==
          player::IntentionalStopKind::Requested);
}

TEST_CASE("buffer phases apply once per load and reject stale generations") {
    player::BufferPhaseGate gate;
    gate.begin_load(core::Generation{4});
    CHECK(gate.begin(core::Generation{4}, core::BufferPhase::Zap));
    CHECK_FALSE(gate.begin(core::Generation{4}, core::BufferPhase::Zap));
    CHECK_FALSE(gate.begin(core::Generation{3}, core::BufferPhase::Steady));
    CHECK_FALSE(gate.settle(core::Generation{4}, core::BufferPhase::Zap,
                            player::BufferPhaseProperty::CacheSeconds, true));
    const auto applied = gate.settle(
        core::Generation{4}, core::BufferPhase::Zap,
        player::BufferPhaseProperty::ReadaheadSeconds, true);
    REQUIRE(applied);
    CHECK(*applied == player::BufferPhaseCommandState::Applied);
    CHECK(gate.state(core::Generation{4}, core::BufferPhase::Zap) ==
          player::BufferPhaseCommandState::Applied);
    CHECK(gate.begin(core::Generation{4}, core::BufferPhase::Steady));

    // A recovery reload is a new load epoch even though it retains generation.
    gate.begin_load(core::Generation{4});
    CHECK(gate.begin(core::Generation{4}, core::BufferPhase::Zap));
    CHECK(gate.begin(core::Generation{4}, core::BufferPhase::Steady));
}

TEST_CASE("buffer phase settles only after both results and exposes partial failure") {
    player::BufferPhaseGate gate;
    gate.begin_load(core::Generation{5});
    REQUIRE(gate.begin(core::Generation{5}, core::BufferPhase::Steady));
    CHECK_FALSE(gate.settle(core::Generation{4}, core::BufferPhase::Steady,
                            player::BufferPhaseProperty::CacheSeconds, true));
    CHECK_FALSE(gate.settle(core::Generation{5}, core::BufferPhase::Steady,
                            player::BufferPhaseProperty::CacheSeconds, true));
    CHECK(gate.state(core::Generation{5}, core::BufferPhase::Steady) ==
          player::BufferPhaseCommandState::Pending);
    const auto failed = gate.settle(
        core::Generation{5}, core::BufferPhase::Steady,
        player::BufferPhaseProperty::ReadaheadSeconds, false);
    REQUIRE(failed);
    CHECK(*failed == player::BufferPhaseCommandState::Failed);
    CHECK(gate.state(core::Generation{5}, core::BufferPhase::Steady) ==
          player::BufferPhaseCommandState::Failed);
    CHECK_FALSE(gate.begin(core::Generation{5}, core::BufferPhase::Steady));

    gate.begin_load(core::Generation{6});
    REQUIRE(gate.begin(core::Generation{6}, core::BufferPhase::Zap));
    const auto immediate_failure = gate.settle(
        core::Generation{6}, core::BufferPhase::Zap,
        player::BufferPhaseProperty::CacheSeconds, false);
    REQUIRE(immediate_failure);
    CHECK(*immediate_failure == player::BufferPhaseCommandState::Failed);
}

TEST_CASE("new loads clear observations but retain lifetime diagnostics") {
    player::Diagnostics diagnostics;
    diagnostics.video_codec = "old-codec";
    diagnostics.hwdec_active = "old-hwdec";
    diagnostics.video_width = 1920;
    diagnostics.video_height = 1080;
    diagnostics.core_idle = true;
    diagnostics.paused_for_cache = true;
    diagnostics.playback_time_seconds = 45.0;
    diagnostics.cache_end_seconds = 54.0;
    diagnostics.cache_duration_seconds = 9.0;
    diagnostics.input_rate_bytes_per_second = 20.0;
    diagnostics.container_fps = 50.0;
    diagnostics.buffer_phase = core::BufferPhase::Steady;
    diagnostics.buffer_commands_accepted = 7;
    diagnostics.mpv_playback_restart_events = 3;
    diagnostics.engine_message_count = 12;
    diagnostics.last_engine_message = player::SanitizedEngineWarning{
        player::EngineWarningComponent::Demuxer,
        player::EngineWarningCategory::CorruptPacket};
    diagnostics.unattributed_engine_message_count = 4;
    diagnostics.last_unattributed_engine_message = player::SanitizedEngineWarning{
        player::EngineWarningComponent::Http,
        player::EngineWarningCategory::NetworkTimeout};
    diagnostics.request_shape = player::inspect_request_shape(
        "https://user:secret@example.invalid/live/u/p/123.ts?token=secret",
        player::LoadRequestIntent::FreshSelection,
        core::RecoveryTransport::MpegTs, false);

    player::reset_load_observations(diagnostics);

    CHECK(diagnostics.video_codec.empty());
    CHECK(diagnostics.hwdec_active.empty());
    CHECK(diagnostics.video_width == 0);
    CHECK(diagnostics.video_height == 0);
    CHECK_FALSE(diagnostics.core_idle);
    CHECK_FALSE(diagnostics.paused_for_cache);
    CHECK_FALSE(diagnostics.playback_time_seconds);
    CHECK_FALSE(diagnostics.cache_end_seconds);
    CHECK_FALSE(diagnostics.cache_duration_seconds);
    CHECK_FALSE(diagnostics.input_rate_bytes_per_second);
    CHECK_FALSE(diagnostics.container_fps);
    CHECK(diagnostics.buffer_phase == core::BufferPhase::Zap);
    CHECK(diagnostics.buffer_phase_command_state ==
          player::BufferPhaseCommandState::Unissued);
    CHECK(diagnostics.buffer_commands_accepted == 7);
    CHECK(diagnostics.mpv_playback_restart_events == 3);
    CHECK(diagnostics.engine_message_count == 0);
    CHECK_FALSE(diagnostics.last_engine_message);
    CHECK(diagnostics.unattributed_engine_message_count == 0);
    CHECK_FALSE(diagnostics.last_unattributed_engine_message);
    CHECK_FALSE(diagnostics.request_shape);
}

TEST_CASE("timeline presentation distinguishes jumps stops pauses and resumes") {
    core::TimelineEvidence evidence;
    evidence.elapsed_seconds = 2.0;
    evidence.playback_movement_seconds = -1.0;
    evidence.playback_deviation_seconds = -3.0;
    CHECK(player::classify_timeline(evidence, core::kDefaultHealthPolicy) ==
          player::TimelineClassification::Backward);

    evidence.playback_movement_seconds = 0.0;
    evidence.playback_deviation_seconds = -2.0;
    CHECK(player::classify_timeline(evidence, core::kDefaultHealthPolicy) ==
          player::TimelineClassification::NoProgress);
    evidence.cache_paused = true;
    CHECK(player::classify_timeline(evidence, core::kDefaultHealthPolicy) ==
          player::TimelineClassification::PausedNoProgress);

    evidence.cache_paused = false;
    evidence.previous_cache_paused = true;
    evidence.playback_movement_seconds = 0.25;
    evidence.playback_deviation_seconds = -1.75;
    CHECK(player::classify_timeline(evidence, core::kDefaultHealthPolicy) ==
          player::TimelineClassification::ResumeLag);

    evidence.previous_cache_paused = false;
    evidence.playback_movement_seconds = 3.0;
    evidence.playback_deviation_seconds = 1.5;
    CHECK(player::classify_timeline(evidence, core::kDefaultHealthPolicy) ==
          player::TimelineClassification::ForwardJump);
    evidence.playback_deviation_seconds = 0.0;
    CHECK(player::classify_timeline(evidence, core::kDefaultHealthPolicy) ==
          player::TimelineClassification::NormalAdvance);

    // At the shipped cadence, a modest behind-wall-clock advance remains raw
    // evidence rather than being mislabeled as a discontinuity-scale lag.
    evidence.elapsed_seconds = core::kDefaultHealthPolicy.sample_interval.count();
    evidence.playback_movement_seconds = 0.25;
    evidence.playback_deviation_seconds = -0.25;
    CHECK(player::classify_timeline(evidence, core::kDefaultHealthPolicy) ==
          player::TimelineClassification::NormalAdvance);

    auto sensitive = core::kDefaultHealthPolicy;
    sensitive.discontinuity_jump_seconds = 0.1;
    CHECK(player::classify_timeline(evidence, sensitive) ==
          player::TimelineClassification::ForwardLag);
}

TEST_CASE("engine warning summaries retain context without provider data") {
    const std::string raw =
        "Continuity check failed for pid 17 at "
        "https://alice:password@provider.invalid/live/alice/password/42.ts?token=secret "
        "Authorization: Bearer super-secret";
    const auto warning = player::sanitize_engine_warning("ffmpeg/demuxer", raw, "error");
    CHECK(warning.component == player::EngineWarningComponent::Demuxer);
    CHECK(warning.category == player::EngineWarningCategory::ContinuityError);
    CHECK(warning.severity == player::EngineLogSeverity::Error);

    const std::string retained = std::string(player::to_string(warning.severity)) + "/" +
                                 player::to_string(warning.component) + "/" +
                                 player::to_string(warning.category);
    for (const std::string_view forbidden : {
             "provider.invalid", "alice", "password", "token", "secret",
             "Authorization", "Bearer", "https://"}) {
        CHECK(retained.find(forbidden) == std::string::npos);
    }

    CHECK(player::sanitize_engine_warning(
              "ffmpeg/video", "Non-monotonous DTS in output stream https://host/?key=x",
              "warn")
              .category == player::EngineWarningCategory::NonMonotonicTimestamp);
    CHECK(player::sanitize_engine_warning(
              "ffmpeg/demuxer", "PES packet size mismatch at http://u:p@host/live/u/p/1.ts",
              "fatal")
              .category == player::EngineWarningCategory::CorruptPacket);
    const auto segment = player::sanitize_engine_warning(
        "ffmpeg/demuxer",
        "hls: keepalive request failed when opening url "
        "https://u:p@host/playlist.m3u8?token=x", "warn");
    CHECK(segment.category == player::EngineWarningCategory::HlsSegment);
    CHECK(segment.severity == player::EngineLogSeverity::Warning);
    CHECK(player::sanitize_engine_warning(
              "ffmpeg/demuxer", "hls: keepalive request failed when parsing playlist", "warn")
              .category == player::EngineWarningCategory::HlsPlaylist);
}

TEST_CASE("engine messages stay unattributed until the target entry is active") {
    const auto old_generation = std::optional{core::Generation{4}};
    const auto new_generation = std::optional{core::Generation{5}};

    CHECK(player::classify_engine_message_attribution(
              false, old_generation, new_generation) ==
          player::EngineMessageAttribution::Unattributed);
    // Recovery keeps the generation, so the START_FILE fence is independently
    // necessary to distinguish its handover window.
    CHECK(player::classify_engine_message_attribution(
              false, new_generation, new_generation) ==
          player::EngineMessageAttribution::Unattributed);
    CHECK(player::classify_engine_message_attribution(
              true, old_generation, new_generation) ==
          player::EngineMessageAttribution::Unattributed);
    CHECK(player::classify_engine_message_attribution(
              true, new_generation, new_generation) ==
          player::EngineMessageAttribution::ActiveEntry);
    CHECK(player::classify_engine_message_attribution(
              true, std::nullopt, new_generation) ==
          player::EngineMessageAttribution::Unattributed);
}

TEST_CASE("engine diagnostic logging keeps one line per triple and attribution") {
    player::EngineDiagnosticLogGate gate;
    const player::SanitizedEngineWarning warning{
        player::EngineWarningComponent::Demuxer,
        player::EngineWarningCategory::CorruptPacket,
        player::EngineLogSeverity::Warning};

    CHECK(gate.first_occurrence(player::EngineMessageAttribution::Unattributed,
                                warning));
    CHECK_FALSE(gate.first_occurrence(player::EngineMessageAttribution::Unattributed,
                                      warning));
    CHECK(gate.first_occurrence(player::EngineMessageAttribution::ActiveEntry,
                                warning));

    auto error = warning;
    error.severity = player::EngineLogSeverity::Error;
    CHECK(gate.first_occurrence(player::EngineMessageAttribution::ActiveEntry,
                                error));
    CHECK_FALSE(gate.first_occurrence(player::EngineMessageAttribution::ActiveEntry,
                                      error));

    gate.reset();
    CHECK(gate.first_occurrence(player::EngineMessageAttribution::ActiveEntry,
                                warning));
}

TEST_CASE("fresh selection request shape is useful and URL free") {
    const std::string raw =
        "https://alice:password@provider.invalid/live/alice/password/42.ts?token=secret";
    const auto shape = player::inspect_request_shape(
        raw, player::LoadRequestIntent::FreshSelection,
        core::RecoveryTransport::MpegTs, false,
        {.provider_session = 3, .channel_session = 7});
    CHECK(shape.intent == player::LoadRequestIntent::FreshSelection);
    CHECK(shape.scheme == player::RequestScheme::Https);
    CHECK(shape.target == player::RequestTargetShape::XtreamLive);
    CHECK(shape.query_present);
    CHECK(shape.userinfo_present);
    CHECK_FALSE(shape.forced_format);
    CHECK(shape.correlation.provider_session == 3);
    CHECK(shape.correlation.channel_session == 7);

    const std::string retained = std::string(player::to_string(shape.intent)) + "/" +
        player::to_string(shape.scheme) + "/" + player::to_string(shape.target);
    for (const std::string_view forbidden : {
             "provider.invalid", "alice", "password", "token", "secret", "https://"}) {
        CHECK(retained.find(forbidden) == std::string::npos);
    }
}

TEST_CASE("session target identities correlate reselection without provider data") {
    player::SessionTargetRegistry registry;
    CHECK(registry.begin_provider_session() == 1);
    const auto first = registry.identify_channel("provider-stream-id-42");
    const auto other = registry.identify_channel("provider-stream-id-99");
    const auto reselected = registry.identify_channel("provider-stream-id-42");
    CHECK(first.provider_session == 1);
    CHECK(first.channel_session == 1);
    CHECK(other.channel_session == 2);
    CHECK(reselected.provider_session == first.provider_session);
    CHECK(reselected.channel_session == first.channel_session);

    CHECK(registry.begin_provider_session() == 2);
    const auto replacement_provider = registry.identify_channel("provider-stream-id-42");
    CHECK(replacement_provider.provider_session == 2);
    CHECK(replacement_provider.channel_session == 1);
}

TEST_CASE("recovery effect executor routes every native action and settles outcomes") {
    std::vector<core::SupervisorEvent> events;
    std::vector<std::string> calls;
    player::RecoveryExecutor executor{
        .reopen_stream = [&](core::Generation) {
            calls.push_back("reopen-stream");
            return std::optional{core::RecoveryTransport::MpegTs};
        },
        .reload_hls_live = [&](core::Generation) {
            calls.push_back("reload-hls-live");
            return std::optional{core::RecoveryTransport::Hls};
        },
        .reopen_probed = [&](core::Generation) {
            calls.push_back("reopen-probed");
            return std::optional{core::RecoveryTransport::MpegTs};
        },
        .recreate_player = [&](core::Generation) {
            calls.push_back("recreate-player");
            return std::optional{core::RecoveryTransport::MpegTs};
        },
    };
    const std::array<core::SupervisorEffectPayload, 4> payloads{
        core::ReopenStream{}, core::ReloadHlsLive{}, core::ReopenProbed{},
        core::RecreatePlayer{}};
    for (const auto& payload : payloads) {
        player::execute_recovery_effect(
            {core::Generation{9}, payload}, &executor,
            [&](const auto& event) { events.push_back(event); });
    }
    CHECK(calls == std::vector<std::string>{"reopen-stream", "reload-hls-live",
                                            "reopen-probed", "recreate-player"});
    REQUIRE(events.size() == 4);
    for (const auto& event : events) {
        CHECK(std::get<core::StreamLoadIssued>(event).generation == core::Generation{9});
    }
}

TEST_CASE("missing rejected and throwing recovery executors terminate instead of parking") {
    std::vector<core::SupervisorEvent> events;
    int rejections = 0;
    const core::SupervisorEffect effect{core::Generation{6}, core::RecreatePlayer{}};
    auto dispatch = [&](const auto& event) { events.push_back(event); };
    auto rejected = [&](const auto&) { ++rejections; };

    player::execute_recovery_effect(effect, nullptr, dispatch, rejected);
    player::RecoveryExecutor empty;
    player::execute_recovery_effect(effect, &empty, dispatch, rejected);
    empty.recreate_player = [](core::Generation) -> std::optional<core::RecoveryTransport> {
        throw 7;
    };
    player::execute_recovery_effect(effect, &empty, dispatch, rejected);
    REQUIRE(events.size() == 3);
    CHECK(rejections == 3);
    for (const auto& event : events) {
        CHECK(std::get<core::SourceFailed>(event).generation == core::Generation{6});
    }
}

TEST_CASE("a stale recreation result cannot replace the current generation") {
    auto state = core::reduce_supervisor_state(
        core::initial_supervisor_state(), core::ChannelRequested{core::Generation{2}},
        core::TimePoint{}).state;
    player::RecoveryExecutor executor;
    executor.recreate_player = [](core::Generation) {
        return std::optional{core::RecoveryTransport::MpegTs};
    };
    player::execute_recovery_effect(
        {core::Generation{1}, core::RecreatePlayer{}}, &executor,
        [&](const core::SupervisorEvent& event) {
            state = core::reduce_supervisor_state(state, event, core::TimePoint{}).state;
        });
    CHECK(state.name == core::SupervisorStateName::Loading);
    CHECK(state.generation == core::Generation{2});
    CHECK_FALSE(state.transport);
}

TEST_CASE("pinned transport log patterns produce only sanitized classifications") {
    const auto classify = [](std::string_view text,
                             core::RecoveryTransport transport = core::RecoveryTransport::MpegTs,
                             bool loaded = false, bool forced = false) {
        return player::classify_transport_log(text, transport, loaded, forced);
    };
    const auto is_failure = [&](std::string_view text,
                                core::TransportFailureReason expected,
                                core::RecoveryTransport transport = core::RecoveryTransport::MpegTs,
                                bool loaded = false, bool forced = false) {
        const auto result = classify(text, transport, loaded, forced);
        return result && std::holds_alternative<core::TransportFailureReason>(*result) &&
               std::get<core::TransportFailureReason>(*result) == expected;
    };
    for (const auto text : {"HTTP error 401 Unauthorized https://user:secret@example.invalid/live",
                            "http_code=401"}) {
        const auto result = classify(text);
        REQUIRE(result);
        CHECK(std::holds_alternative<player::AuthenticationRejected>(*result));
    }
    for (const auto text : {"connection timed out", "Operation timed out",
                            "request timed out",
                            "Error reading HTTP response: Error number -138 occurred"}) {
        CHECK(is_failure(text, core::TransportFailureReason::HttpRequestTimeout));
    }
    for (const auto text : {"HTTP error 503 Service Unavailable",
                            "Error reading HTTP response: Error number -10054 occurred",
                            "hls: keepalive request failed when parsing playlist"}) {
        CHECK(is_failure(text, core::TransportFailureReason::HlsPlaylistFailed,
                         core::RecoveryTransport::Hls));
    }
    for (const auto text : {"failed to open media segment",
                            "unable to open next segment",
                            "hls: keepalive request failed when opening url"}) {
        CHECK(is_failure(text, core::TransportFailureReason::HlsSegmentUnavailable,
                         core::RecoveryTransport::Hls));
    }
    for (const auto text : {"Failed to recognize file format",
                            "Could not determine the input format",
                            "Could not find format"}) {
        CHECK(is_failure(text, core::TransportFailureReason::FormatProbeRequired));
    }
    CHECK(is_failure("Invalid data found when processing input",
                     core::TransportFailureReason::FormatProbeRequired));
    CHECK_FALSE(classify("Invalid data found when processing input",
                         core::RecoveryTransport::MpegTs, true, false));
    CHECK_FALSE(classify("Failed to recognize file format",
                         core::RecoveryTransport::MpegTs, false, true));
    CHECK_FALSE(classify("HTTP error 403 Forbidden"));
    CHECK_FALSE(classify("generic network error"));
    CHECK_FALSE(classify("HTTP error 503 Service Unavailable",
                         core::RecoveryTransport::MpegTs));
}

TEST_CASE("transport classifications remain generation-scoped adapter edges") {
    player::PlayerEventAdapter adapter;
    adapter.authentication_rejected(core::Generation{12});
    adapter.transport_failure(core::Generation{13},
                              core::TransportFailureReason::HlsPlaylistFailed);
    const auto events = adapter.drain();
    REQUIRE(events.size() == 2);
    CHECK(events[0].generation == core::Generation{12});
    CHECK(std::holds_alternative<player::PlayerAuthenticationRejected>(events[0].payload));
    CHECK(events[1].generation == core::Generation{13});
    CHECK(std::get<player::TransportFailureDetected>(events[1].payload).reason ==
          core::TransportFailureReason::HlsPlaylistFailed);
}
