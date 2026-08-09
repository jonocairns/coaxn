#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>

#include "core/supervisor.hpp"

using namespace coax::core;
using Catch::Approx;

namespace {
TimePoint at(double seconds_value) { return TimePoint{seconds(seconds_value)}; }

SupervisorReduction apply(const SupervisorState& state, const SupervisorEvent& event,
                          double seconds_value) {
    return reduce_supervisor_state(state, event, at(seconds_value));
}
SupervisorState step(const SupervisorState& state, const SupervisorEvent& event,
                     double seconds_value) {
    return apply(state, event, seconds_value).state;
}
SupervisorState reach_steady(Generation generation = Generation{1},
                             RecoveryTransport transport = RecoveryTransport::MpegTs) {
    auto state = step(initial_supervisor_state(), ChannelRequested{generation}, 0.0);
    state = step(state, StreamLoadIssued{generation, LoadAttempt{1}, LoadIntent::FreshSelection, transport}, 0.01);
    state = step(state, FirstFrame{generation, LoadAttempt{1}}, 1.0);
    state = step(state, PlaybackHealthObserved{generation, LoadAttempt{1}, false}, 1.0);
    return step(state, DeadlineReached{}, 6.0);
}
RecoveryAction recovery(const SupervisorState& state) {
    REQUIRE(state.recovery);
    return *state.recovery;
}
}  // namespace

TEST_CASE("recovery schedule budget phases and versions are pinned") {
    const std::array<double, 5> expected{0.5, 1.0, 2.0, 4.0, 5.0};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(kDefaultRecoveryPolicy.attempt_delays[i].count() == expected[i]);
    }
    CHECK(kDefaultRecoveryPolicy.wall_clock_budget == seconds(30));
    CHECK(kDefaultRecoveryPolicy.steady_healthy_window == seconds(5));
    CHECK(kDefaultRecoveryPolicy.short_reopens_before_recreation == 2);
    CHECK(kDefaultRecoveryPolicy.version == "coax-recovery-v3");
    CHECK(kTransportPolicyVersion == "coax-transport-recovery-v7");
}

TEST_CASE("idle owns no stream and clean playback reaches steady after five seconds") {
    const auto idle = initial_supervisor_state();
    CHECK_FALSE(apply(idle, ProcessExited{Generation{1}, LoadAttempt{1}}, 1).transition);
    CHECK_FALSE(apply(idle, IpcUnresponsive{Generation{1}, LoadAttempt{1}}, 1).transition);
    CHECK_FALSE(apply(idle, StreamEnded{Generation{1}, LoadAttempt{1}, EndReason::Error, {}}, 1).transition);

    auto state = step(idle, ChannelRequested{Generation{1}}, 0);
    CHECK(state.name == SupervisorStateName::Loading);
    CHECK_FALSE(state.transport);
    state = step(state, StreamLoadIssued{Generation{1}, LoadAttempt{1}, LoadIntent::FreshSelection, RecoveryTransport::MpegTs}, .01);
    CHECK(state.name == SupervisorStateName::Zap);
    state = step(state, PlaybackHealthObserved{
        Generation{1}, LoadAttempt{1}, true}, .5);
    CHECK(state.playback_unhealthy);
    state = step(state, FirstFrame{Generation{1}, LoadAttempt{1}}, 1);
    CHECK_FALSE(state.playback_unhealthy);
    CHECK(next_deadline_at(state) == at(6));
    CHECK_FALSE(apply(state, DeadlineReached{}, 5.999).transition);
    state = step(state, DeadlineReached{}, 6);
    CHECK(state.name == SupervisorStateName::Steady);
    CHECK_FALSE(next_deadline_at(state));
}

TEST_CASE("continuous TS terminal end schedules and emits a generation-scoped reopen") {
    auto result = apply(reach_steady(), StreamEnded{Generation{1}, LoadAttempt{1}, EndReason::Error, {}}, 10);
    CHECK(result.state.name == SupervisorStateName::Recovering);
    CHECK(result.state.attempt == 1);
    CHECK(result.state.detection == DetectionReason::StreamEndedError);
    CHECK(recovery(result.state) == RecoveryAction::ReopenStream);
    CHECK(next_deadline_at(result.state) == at(10.5));
    CHECK(result.effects.empty());

    result = apply(result.state, DeadlineReached{}, 10.5);
    REQUIRE(result.effects.size() == 1);
    CHECK(result.effects[0].generation == Generation{1});
    CHECK(std::holds_alternative<ReopenStream>(result.effects[0].payload));
    CHECK_FALSE(next_deadline_at(result.state));
}

TEST_CASE("unexpected active live EOF reopens the current TS load") {
    auto result = apply(reach_steady(), StreamEnded{
        Generation{1}, LoadAttempt{1}, EndReason::Eof, {}}, 10);
    CHECK(result.state.name == SupervisorStateName::Recovering);
    CHECK(result.state.detection == DetectionReason::StreamEndedEof);
    CHECK(recovery(result.state) == RecoveryAction::ReopenStream);
    result = apply(result.state, DeadlineReached{}, 10.5);
    REQUIRE(result.effects.size() == 1);
    CHECK(result.effects[0].load_attempt == LoadAttempt{2});
    CHECK(std::holds_alternative<ReopenStream>(result.effects[0].payload));
}

TEST_CASE("attempt budget resets only after recovered playback is steady") {
    auto state = step(reach_steady(), StreamEnded{Generation{1}, LoadAttempt{1}, EndReason::Error, {}}, 10);
    state = step(state, DeadlineReached{}, 10.5);
    state = step(state, StreamLoadIssued{Generation{1}, LoadAttempt{2}, LoadIntent::RecoveryReopen, RecoveryTransport::MpegTs}, 10.51);
    state = step(state, FirstFrame{Generation{1}, LoadAttempt{2}}, 11.2);
    state = step(state, PlaybackHealthObserved{Generation{1}, LoadAttempt{2}, false}, 11.2);
    CHECK(state.attempt == 1);
    state = step(state, DeadlineReached{}, 16.2);
    CHECK(state.name == SupervisorStateName::Steady);
    CHECK(state.attempt == 0);
    CHECK_FALSE(state.recovery_started_at);
    CHECK_FALSE(state.detection);
}

TEST_CASE("same-attempt first frame cancels an unissued opening retry and starts probation") {
    auto state = step(initial_supervisor_state(), ChannelRequested{Generation{1}}, 0);
    state = step(state, StreamLoadIssued{
        Generation{1}, LoadAttempt{1}, LoadIntent::FreshSelection,
        RecoveryTransport::MpegTs}, .01);
    state = step(state, PlaybackHealthObserved{
        Generation{1}, LoadAttempt{1}, true}, .5);
    REQUIRE(state.playback_unhealthy);

    auto stalled = apply(state, PlaybackStalled{
        Generation{1}, LoadAttempt{1}, StallKind::Open}, 8.1);
    REQUIRE(stalled.state.name == SupervisorStateName::Recovering);
    REQUIRE(stalled.state.recovery_started_at == at(8.1));
    REQUIRE(next_deadline_at(stalled.state) == at(8.6));
    CHECK(stalled.state.attempt == 1);
    CHECK(stalled.effects.empty());

    auto started = apply(stalled.state, FirstFrame{
        Generation{1}, LoadAttempt{1}}, 8.4);
    REQUIRE(started.transition);
    CHECK(started.transition->outcome == RecoveryOutcome::LateFirstFrame);
    CHECK(started.transition->reason == "first-frame-cancelled-opening-retry");
    CHECK(started.state.name == SupervisorStateName::Zap);
    CHECK(started.state.attempt == 1);
    CHECK(started.state.recovery_started_at == at(8.1));
    CHECK_FALSE(started.state.playback_unhealthy);
    CHECK(next_deadline_at(started.state) == at(13.4));
    CHECK(started.effects.empty());

    // The cancelled retry did not declare success. The original deadline can
    // no longer issue its source replacement, and probation is still armed.
    CHECK_FALSE(apply(started.state, DeadlineReached{}, 8.6).transition);
    CHECK(started.state.name != SupervisorStateName::Steady);
    CHECK(step(started.state, DeadlineReached{}, 13.4).name ==
          SupervisorStateName::Steady);

    auto failed = apply(started.state, StreamEnded{
        Generation{1}, LoadAttempt{1}, EndReason::Eof, {}}, 9.0);
    REQUIRE(failed.transition);
    CHECK(failed.transition->outcome == RecoveryOutcome::RenewedEof);
    REQUIRE(failed.transition->first_frame_to_outcome);
    CHECK(failed.transition->first_frame_to_outcome->count() == Approx(.6));
    CHECK(failed.state.name == SupervisorStateName::Recovering);
    CHECK(failed.state.attempt == 2);
    CHECK(failed.state.recovery_started_at == at(8.1));
    CHECK(failed.effects.empty());
}

TEST_CASE("a cancelled retry counts a short recovery load at most once") {
    auto state = step(reach_steady(), StreamEnded{
        Generation{1}, LoadAttempt{1}, EndReason::Eof, {}}, 10);
    state = step(state, DeadlineReached{}, 10.5);
    state = step(state, StreamLoadIssued{
        Generation{1}, LoadAttempt{2}, LoadIntent::RecoveryReopen,
        RecoveryTransport::MpegTs}, 10.51);

    state = step(state, PlaybackStalled{
        Generation{1}, LoadAttempt{2}, StallKind::Open}, 18.6);
    CHECK(state.short_recovery_load_failures == 1);
    state = step(state, FirstFrame{Generation{1}, LoadAttempt{2}}, 19.0);
    state = step(state, StreamEnded{
        Generation{1}, LoadAttempt{2}, EndReason::Eof, {}}, 20.0);
    CHECK(state.short_recovery_load_failures == 1);
    CHECK_FALSE(state.short_load_recreation_used);
    CHECK(recovery(state) == RecoveryAction::ReopenStream);
}

TEST_CASE("first frame followed by pre-probation EOF retains the recovery episode") {
    auto state = step(reach_steady(), ForwardProgressObserved{
        Generation{1}, LoadAttempt{1}}, 9);
    state = step(state, StreamEnded{
        Generation{1}, LoadAttempt{1}, EndReason::Eof, {}}, 10);
    const auto episode_started = state.recovery_started_at;
    state = step(state, DeadlineReached{}, 10.5);
    state = step(state, StreamLoadIssued{
        Generation{1}, LoadAttempt{2}, LoadIntent::RecoveryReopen,
        RecoveryTransport::MpegTs}, 10.51);
    state = step(state, FirstFrame{Generation{1}, LoadAttempt{2}}, 11);

    const auto failed = apply(state, StreamEnded{
        Generation{1}, LoadAttempt{2}, EndReason::Eof, {}}, 12);
    CHECK(failed.state.name == SupervisorStateName::Recovering);
    CHECK(failed.state.attempt == 2);
    CHECK(failed.state.recovery_started_at == episode_started);
    CHECK(failed.state.short_recovery_load_failures == 1);
    REQUIRE(failed.transition);
    CHECK(failed.transition->outcome == RecoveryOutcome::RenewedEof);
    CHECK(failed.transition->first_frame_to_outcome == seconds(1));
    REQUIRE(failed.transition->recovered_load_lifetime);
    CHECK(failed.transition->recovered_load_lifetime->count() == Approx(1.49));
}

TEST_CASE("repeated short recovered loads recreate once without resetting budget") {
    auto state = step(reach_steady(), StreamEnded{
        Generation{1}, LoadAttempt{1}, EndReason::Eof, {}}, 10);
    const auto episode_started = state.recovery_started_at;

    state = step(state, DeadlineReached{}, 10.5);
    state = step(state, StreamLoadIssued{
        Generation{1}, LoadAttempt{2}, LoadIntent::RecoveryReopen,
        RecoveryTransport::MpegTs}, 10.51);
    state = step(state, FirstFrame{Generation{1}, LoadAttempt{2}}, 10.8);
    state = step(state, StreamEnded{
        Generation{1}, LoadAttempt{2}, EndReason::Eof, {}}, 11);
    CHECK(recovery(state) == RecoveryAction::ReopenStream);

    state = step(state, DeadlineReached{}, 12);
    state = step(state, StreamLoadIssued{
        Generation{1}, LoadAttempt{3}, LoadIntent::RecoveryReopen,
        RecoveryTransport::MpegTs}, 12.01);
    state = step(state, FirstFrame{Generation{1}, LoadAttempt{3}}, 12.2);
    state = step(state, PlaybackStalled{
        Generation{1}, LoadAttempt{3}, StallKind::Progress}, 13);
    CHECK(recovery(state) == RecoveryAction::RecreatePlayer);
    CHECK(state.short_load_recreation_used);
    CHECK(state.attempt == 3);
    CHECK(state.recovery_started_at == episode_started);

    auto fired = apply(state, DeadlineReached{}, 15);
    REQUIRE(fired.effects.size() == 1);
    CHECK(fired.effects[0].load_attempt == LoadAttempt{4});
    CHECK(std::holds_alternative<RecreatePlayer>(fired.effects[0].payload));
    state = step(fired.state, StreamLoadIssued{
        Generation{1}, LoadAttempt{4}, LoadIntent::PlayerRecreation,
        RecoveryTransport::MpegTs}, 15.01);
    CHECK(state.attempt == 3);
    CHECK(state.recovery_started_at == episode_started);
    const auto stats = project_supervisor_stats(state, at(15.01));
    REQUIRE(stats.elapsed_budget);
    CHECK(stats.elapsed_budget->count() == Approx(5.01));

    // Failure after recreation returns to source reopen. The one-time
    // escalation flag and original episode remain intact.
    state = step(state, FirstFrame{Generation{1}, LoadAttempt{4}}, 15.2);
    state = step(state, StreamEnded{
        Generation{1}, LoadAttempt{4}, EndReason::Eof, {}}, 16);
    CHECK(recovery(state) == RecoveryAction::ReopenStream);
    CHECK(state.short_load_recreation_used);
    CHECK(state.recovery_started_at == episode_started);
}

TEST_CASE("backend failures map to in-process player recreation for both transports") {
    auto state = step(reach_steady(), ProcessExited{Generation{1}, LoadAttempt{1}}, 10);
    CHECK(recovery(state) == RecoveryAction::RecreatePlayer);
    auto fired = apply(state, DeadlineReached{}, 10.5);
    REQUIRE(fired.effects.size() == 1);
    CHECK(std::holds_alternative<RecreatePlayer>(fired.effects[0].payload));

    state = step(reach_steady(Generation{1}, RecoveryTransport::Hls),
                 IpcUnresponsive{Generation{1}, LoadAttempt{1}}, 10);
    CHECK(recovery(state) == RecoveryAction::RecreatePlayer);
    CHECK(state.detection == DetectionReason::IpcUnresponsive);
}

TEST_CASE("recreation faults upgrade a weaker recovery already in backoff") {
    const std::array<std::pair<SupervisorEvent, DetectionReason>, 3> faults{
        std::pair<SupervisorEvent, DetectionReason>{
            PresentationLost{Generation{1}}, DetectionReason::PresentationDeviceLost},
        std::pair<SupervisorEvent, DetectionReason>{
            ProcessExited{Generation{1}, LoadAttempt{1}}, DetectionReason::ProcessExited},
        std::pair<SupervisorEvent, DetectionReason>{
            IpcUnresponsive{Generation{1}, LoadAttempt{1}},
            DetectionReason::IpcUnresponsive},
    };
    for (const auto& [fault, expected] : faults) {
        auto state = step(reach_steady(), StreamEnded{
            Generation{1}, LoadAttempt{1}, EndReason::Error, {}}, 10);
        REQUIRE(state.name == SupervisorStateName::Recovering);
        REQUIRE(recovery(state) == RecoveryAction::ReopenStream);
        REQUIRE(state.attempt == 1);

        auto upgraded = apply(state, fault, 10.1);
        REQUIRE(upgraded.transition);
        CHECK(upgraded.state.name == SupervisorStateName::Recovering);
        CHECK(upgraded.state.detection == expected);
        CHECK(recovery(upgraded.state) == RecoveryAction::RecreatePlayer);
        CHECK(upgraded.state.attempt == 2);
        CHECK(upgraded.state.recovery_started_at == state.recovery_started_at);
        CHECK(next_deadline_at(upgraded.state) == at(11.1));

        // Once recreation is pending, duplicate recreation-class faults and
        // ordinary load faults cannot spend further attempts.
        CHECK_FALSE(apply(upgraded.state, fault, 10.2).transition);
        CHECK_FALSE(apply(upgraded.state, PlaybackStalled{
            Generation{1}, LoadAttempt{1}, StallKind::Progress}, 10.2).transition);
    }
}

TEST_CASE("a late frame cannot cancel backend IPC or presentation recreation") {
    const std::array<SupervisorEvent, 3> faults{
        PresentationLost{Generation{1}},
        ProcessExited{Generation{1}, LoadAttempt{1}},
        IpcUnresponsive{Generation{1}, LoadAttempt{1}},
    };
    for (const auto& fault : faults) {
        auto state = step(reach_steady(), PlaybackStalled{
            Generation{1}, LoadAttempt{1}, StallKind::Open}, 10);
        state = step(state, fault, 10.1);
        REQUIRE(state.recovery == RecoveryAction::RecreatePlayer);
        const auto deadline = next_deadline_at(state);
        REQUIRE(deadline);

        const auto late = apply(state, FirstFrame{
            Generation{1}, LoadAttempt{1}}, 10.2);
        CHECK_FALSE(late.transition);
        CHECK(late.state.name == SupervisorStateName::Recovering);
        CHECK(late.state.recovery == RecoveryAction::RecreatePlayer);
        CHECK(next_deadline_at(late.state) == deadline);
    }
}

TEST_CASE("a stale in-flight reopen cannot discard a recreation upgrade") {
    auto state = step(reach_steady(), StreamEnded{
        Generation{1}, LoadAttempt{1}, EndReason::Error, {}}, 10);
    auto fired = apply(state, DeadlineReached{}, 10.5);
    REQUIRE(fired.effects.size() == 1);
    CHECK(std::holds_alternative<ReopenStream>(fired.effects[0].payload));
    CHECK(fired.effects[0].load_attempt == LoadAttempt{2});
    REQUIRE(fired.state.pending_load_attempt == LoadAttempt{2});

    auto upgraded = apply(fired.state, PresentationLost{Generation{1}}, 10.6);
    REQUIRE(upgraded.transition);
    CHECK(recovery(upgraded.state) == RecoveryAction::RecreatePlayer);
    CHECK(upgraded.state.attempt == 2);
    CHECK(upgraded.state.pending_load_attempt == LoadAttempt{3});
    CHECK(upgraded.state.pending_load_intent == LoadIntent::PlayerRecreation);
    REQUIRE(next_deadline_at(upgraded.state) == at(11.6));

    const auto stale = apply(upgraded.state, StreamLoadIssued{
        Generation{1}, LoadAttempt{2}, LoadIntent::RecoveryReopen,
        RecoveryTransport::MpegTs}, 10.7);
    CHECK_FALSE(stale.transition);
    CHECK(stale.state.name == SupervisorStateName::Recovering);
    CHECK(stale.state.pending_load_attempt == LoadAttempt{3});
    CHECK(next_deadline_at(stale.state) == at(11.6));

    const auto stale_failure = apply(stale.state, SourceFailed{
        Generation{1}, LoadAttempt{2}}, 10.8);
    CHECK_FALSE(stale_failure.transition);
    CHECK(stale_failure.state.name == SupervisorStateName::Recovering);
    CHECK(stale_failure.state.pending_load_attempt == LoadAttempt{3});
    CHECK(next_deadline_at(stale_failure.state) == at(11.6));

    const auto recreation = apply(stale_failure.state, DeadlineReached{}, 11.6);
    REQUIRE(recreation.effects.size() == 1);
    CHECK(std::holds_alternative<RecreatePlayer>(recreation.effects[0].payload));
    CHECK(recreation.effects[0].load_attempt == LoadAttempt{3});

    state = step(recreation.state, StreamLoadIssued{
        Generation{1}, LoadAttempt{3}, LoadIntent::PlayerRecreation,
        RecoveryTransport::MpegTs}, 11.7);
    state = step(state, StreamEnded{
        Generation{1}, LoadAttempt{3}, EndReason::Error, {}}, 12);
    const auto following = apply(state, DeadlineReached{}, 14);
    REQUIRE(following.effects.size() == 1);
    CHECK(std::holds_alternative<ReopenStream>(following.effects[0].payload));
    CHECK(following.effects[0].load_attempt == LoadAttempt{4});
}

TEST_CASE("presentation loss recreates the player through the shared recovery path") {
    // The surface is rebuilt by the platform adapter; what reaches the
    // supervisor is a libmpv instance still holding the device that died.
    auto result = apply(reach_steady(), PresentationLost{Generation{1}}, 10);
    CHECK(result.state.name == SupervisorStateName::Recovering);
    CHECK(result.state.detection == DetectionReason::PresentationDeviceLost);
    CHECK(recovery(result.state) == RecoveryAction::RecreatePlayer);
    // Bounding and pacing are inherited, not reimplemented: the same attempt
    // schedule as every other fault.
    CHECK(result.state.attempt == 1);
    CHECK(next_deadline_at(result.state) == at(10.5));
    CHECK(result.effects.empty());

    result = apply(result.state, DeadlineReached{}, 10.5);
    REQUIRE(result.effects.size() == 1);
    CHECK(result.effects[0].generation == Generation{1});
    CHECK(std::holds_alternative<RecreatePlayer>(result.effects[0].payload));

    // HLS is no different: the device is gone either way.
    const auto hls = step(reach_steady(Generation{1}, RecoveryTransport::Hls),
                          PresentationLost{Generation{1}}, 10);
    CHECK(recovery(hls) == RecoveryAction::RecreatePlayer);
}

TEST_CASE("presentation rebuilds are bounded and cannot complete for a stale generation") {
    // A rebuild armed for one channel must not resume it once a newer channel
    // has been asked for. The rebuild takes real time, so the newer request
    // can easily arrive in between.
    auto armed = step(reach_steady(), PresentationLost{Generation{1}}, 10);
    REQUIRE(next_deadline_at(armed) == at(10.5));
    const auto superseded = step(armed, ChannelRequested{Generation{2}}, 10.2);
    CHECK(superseded.name == SupervisorStateName::Loading);
    CHECK_FALSE(next_deadline_at(superseded));
    CHECK(apply(superseded, DeadlineReached{}, 10.5).effects.empty());
    // And the loss itself, reported late for the channel that is gone.
    CHECK_FALSE(apply(superseded, PresentationLost{Generation{1}}, 10.6).transition);

    // A device that keeps dying exhausts the shared attempt budget and stops.
    auto state = reach_steady();
    double now = 10.0;
    for (std::size_t attempt = 0; attempt < kDefaultRecoveryPolicy.attempt_delays.size();
         ++attempt) {
        state = step(state, PresentationLost{Generation{1}}, now);
        CHECK(state.attempt == attempt + 1);
        now += kDefaultRecoveryPolicy.attempt_delays[attempt].count();
        state = step(state, DeadlineReached{}, now);
        REQUIRE(state.pending_load_attempt);
        state = step(state, StreamLoadIssued{
            Generation{1}, *state.pending_load_attempt, LoadIntent::PlayerRecreation,
            RecoveryTransport::MpegTs}, now);
    }
    const auto exhausted = apply(state, PresentationLost{Generation{1}}, now);
    CHECK(exhausted.state.name == SupervisorStateName::Failed);
    CHECK(exhausted.state.failure == FailureReason::AttemptsExhausted);
    CHECK(exhausted.effects.empty());
    // Failed is terminal until a new channel is chosen: it does not loop.
    CHECK_FALSE(apply(exhausted.state, PresentationLost{Generation{1}}, now + 1).transition);
}

TEST_CASE("HLS failure classes and stalls reload the advertised live edge") {
    for (const auto reason : {TransportFailureReason::HlsPlaylistFailed,
                              TransportFailureReason::HlsSegmentUnavailable,
                              TransportFailureReason::HttpRequestTimeout}) {
        auto result = apply(reach_steady(Generation{1}, RecoveryTransport::Hls),
                            StreamEnded{Generation{1}, LoadAttempt{1}, EndReason::Error, reason}, 10);
        CHECK(recovery(result.state) == RecoveryAction::ReloadHlsLive);
        auto fired = apply(result.state, DeadlineReached{}, 10.5);
        REQUIRE(fired.effects.size() == 1);
        CHECK(std::holds_alternative<ReloadHlsLive>(fired.effects[0].payload));
    }
    auto state = step(reach_steady(Generation{1}, RecoveryTransport::Hls),
                      PlaybackStalled{Generation{1}, LoadAttempt{1}, StallKind::Open}, 20);
    CHECK(state.detection == DetectionReason::OpenStall);
    CHECK(recovery(state) == RecoveryAction::ReloadHlsLive);
}

TEST_CASE("five attempts share one episode then terminate") {
    auto state = reach_steady();
    double now = 10.0;
    std::array<double, 5> delays{};
    LoadAttempt active_load{1};
    for (std::size_t attempt = 0; attempt < delays.size(); ++attempt) {
        state = step(state, StreamEnded{Generation{1}, active_load, EndReason::Error, {}}, now);
        CHECK(state.attempt == attempt + 1);
        REQUIRE(next_deadline_at(state));
        delays[attempt] = (next_deadline_at(state).value() - at(now)).count();
        now += delays[attempt];
        state = step(state, DeadlineReached{}, now);
        REQUIRE(state.pending_load_attempt);
        REQUIRE(state.pending_load_intent);
        active_load = *state.pending_load_attempt;
        state = step(state, StreamLoadIssued{
            Generation{1}, active_load, *state.pending_load_intent,
            RecoveryTransport::MpegTs}, now);
    }
    CHECK(delays == std::array<double, 5>{0.5, 1.0, 2.0, 4.0, 5.0});
    const auto result = apply(state, StreamEnded{
        Generation{1}, active_load, EndReason::Error, {}}, now);
    CHECK(result.state.name == SupervisorStateName::Failed);
    CHECK(result.state.failure == FailureReason::AttemptsExhausted);
    CHECK(result.effects.empty());
}

TEST_CASE("an attempt outside the thirty second budget is not started") {
    auto state = step(reach_steady(), StreamEnded{Generation{1}, LoadAttempt{1}, EndReason::Error, {}}, 0);
    CHECK(state.recovery_started_at == at(0));
    state = step(state, DeadlineReached{}, .5);
    state = step(state, StreamLoadIssued{
        Generation{1}, LoadAttempt{2}, LoadIntent::RecoveryReopen,
        RecoveryTransport::MpegTs}, .5);
    const auto result = apply(state, StreamEnded{
        Generation{1}, LoadAttempt{2}, EndReason::Error, {}}, 29.8);
    CHECK(result.state.name == SupervisorStateName::Failed);
    CHECK(result.state.failure == FailureReason::BudgetExpired);
    CHECK(result.state.attempt == 1);
}

TEST_CASE("a late deadline cannot issue a command after the episode budget") {
    auto state = step(reach_steady(), StreamEnded{
        Generation{1}, LoadAttempt{1}, EndReason::Eof, {}}, 0);
    const auto result = apply(state, DeadlineReached{}, 30.1);
    CHECK(result.state.name == SupervisorStateName::Failed);
    CHECK(result.state.failure == FailureReason::BudgetExpired);
    CHECK(result.effects.empty());
    REQUIRE(result.transition);
    CHECK(result.transition->outcome == RecoveryOutcome::TerminalFailure);
}

TEST_CASE("the exact exhausted current load gets one bounded late probation") {
    auto state = step(reach_steady(), StreamEnded{
        Generation{1}, LoadAttempt{1}, EndReason::Eof, {}}, 10);
    state = step(state, DeadlineReached{}, 10.5);
    state = step(state, StreamLoadIssued{
        Generation{1}, LoadAttempt{2}, LoadIntent::RecoveryReopen,
        RecoveryTransport::MpegTs}, 10.51);

    const auto exhausted = apply(state, PlaybackStalled{
        Generation{1}, LoadAttempt{2}, StallKind::Open}, 39.6);
    REQUIRE(exhausted.state.name == SupervisorStateName::Failed);
    REQUIRE(exhausted.state.failure == FailureReason::BudgetExpired);
    CHECK(exhausted.state.late_completion_available);
    CHECK(exhausted.effects.empty());
    CHECK_FALSE(next_deadline_at(exhausted.state));
    CHECK_FALSE(supervisor_health_supervision_enabled(exhausted.state.name));
    CHECK(apply(exhausted.state, DeadlineReached{}, 45).effects.empty());

    // Neither side of the two-dimensional fence can revive Failed.
    CHECK_FALSE(apply(exhausted.state, FirstFrame{
        Generation{2}, LoadAttempt{2}}, 50).transition);
    CHECK_FALSE(apply(exhausted.state, FirstFrame{
        Generation{1}, LoadAttempt{1}}, 50).transition);

    const auto ended_before_frame = apply(exhausted.state, StreamEnded{
        Generation{1}, LoadAttempt{2}, EndReason::Eof, {}}, 49);
    REQUIRE(ended_before_frame.transition);
    CHECK_FALSE(ended_before_frame.state.late_completion_available);
    CHECK_FALSE(apply(ended_before_frame.state, FirstFrame{
        Generation{1}, LoadAttempt{2}}, 50).transition);

    const auto revived = apply(exhausted.state, FirstFrame{
        Generation{1}, LoadAttempt{2}}, 50);
    REQUIRE(revived.transition);
    CHECK(revived.transition->outcome == RecoveryOutcome::LateFirstFrame);
    CHECK(revived.transition->reason == "late-first-frame-after-command-exhaustion");
    CHECK(revived.transition->command_to_first_frame == seconds(39.49));
    CHECK(revived.state.name == SupervisorStateName::Zap);
    CHECK(revived.state.attempt == 1);
    CHECK(revived.state.recovery_started_at == at(10));
    CHECK(revived.state.late_completion_probation);
    CHECK_FALSE(revived.state.late_completion_available);
    CHECK(supervisor_health_supervision_enabled(revived.state.name));
    CHECK(next_deadline_at(revived.state) == at(55));
    CHECK(revived.effects.empty());

    const auto failed = apply(revived.state, StreamEnded{
        Generation{1}, LoadAttempt{2}, EndReason::Eof, {}}, 51);
    REQUIRE(failed.transition);
    CHECK(failed.state.name == SupervisorStateName::Failed);
    CHECK(failed.state.failure == FailureReason::BudgetExpired);
    CHECK_FALSE(failed.state.late_completion_available);
    CHECK(failed.effects.empty());
    CHECK(failed.transition->first_frame_to_outcome == seconds(1));
    REQUIRE(failed.transition->recovered_load_lifetime);
    CHECK(failed.transition->recovered_load_lifetime->count() == Approx(40.49));

    // A failed late probation consumes the one-shot admission. Repeated edges
    // and polls cannot create Failed/revival/retry churn or another command.
    state = failed.state;
    for (int i = 0; i < 8; ++i) {
        const auto frame = apply(state, FirstFrame{
            Generation{1}, LoadAttempt{2}}, 52 + i);
        CHECK_FALSE(frame.transition);
        CHECK(frame.effects.empty());
        const auto ended = apply(frame.state, StreamEnded{
            Generation{1}, LoadAttempt{2}, EndReason::Eof, {}}, 52.1 + i);
        CHECK_FALSE(ended.transition);
        CHECK(ended.effects.empty());
        state = ended.state;
    }
}

TEST_CASE("clean late probation resets the episode and future telemetry") {
    auto state = step(reach_steady(), StreamEnded{
        Generation{1}, LoadAttempt{1}, EndReason::Eof, {}}, 10);
    state = step(state, DeadlineReached{}, 10.5);
    state = step(state, StreamLoadIssued{
        Generation{1}, LoadAttempt{2}, LoadIntent::RecoveryReopen,
        RecoveryTransport::MpegTs}, 10.51);
    state = step(state, PlaybackStalled{
        Generation{1}, LoadAttempt{2}, StallKind::Open}, 39.6);
    state = step(state, FirstFrame{Generation{1}, LoadAttempt{2}}, 50);

    const auto clean = apply(state, DeadlineReached{}, 55);
    REQUIRE(clean.transition);
    CHECK(clean.transition->outcome == RecoveryOutcome::CleanProbation);
    CHECK(clean.transition->first_frame_to_outcome == seconds(5));
    CHECK(clean.state.name == SupervisorStateName::Steady);
    CHECK(clean.state.attempt == 0);
    CHECK_FALSE(clean.state.recovery_started_at);
    CHECK_FALSE(clean.state.first_frame_at);
    CHECK_FALSE(clean.state.late_completion_probation);

    const auto new_fault = apply(clean.state, PlaybackStalled{
        Generation{1}, LoadAttempt{2}, StallKind::Cache}, 100);
    REQUIRE(new_fault.transition);
    CHECK(new_fault.transition->outcome == RecoveryOutcome::FaultDecided);
    CHECK_FALSE(new_fault.transition->first_frame_to_outcome);
    REQUIRE(new_fault.transition->recovered_load_lifetime);
    CHECK(new_fault.transition->recovered_load_lifetime->count() == Approx(89.49));
}

TEST_CASE("terminal auth and source failures wait for a newer generation") {
    auto rejected = apply(reach_steady(), AuthRejected{
        Generation{1}, LoadAttempt{1}}, 10);
    REQUIRE(rejected.transition);
    CHECK(rejected.transition->outcome == RecoveryOutcome::TerminalFailure);
    CHECK(rejected.transition->escalation == RecoveryEscalation::None);
    auto state = rejected.state;
    CHECK(state.name == SupervisorStateName::Failed);
    CHECK(state.failure == FailureReason::AuthRejected);
    CHECK_FALSE(apply(state, AuthRejected{Generation{1}, LoadAttempt{1}}, 11).transition);
    CHECK_FALSE(apply(state, SourceFailed{Generation{1}, LoadAttempt{1}}, 11).transition);
    CHECK_FALSE(apply(state, ProcessExited{Generation{1}, LoadAttempt{1}}, 12).transition);
    CHECK_FALSE(apply(state, DeadlineReached{}, 12).transition);
    state = step(state, ChannelRequested{Generation{2}}, 13);
    CHECK(state.name == SupervisorStateName::Loading);
    CHECK_FALSE(state.failure);

    auto loading = step(initial_supervisor_state(), ChannelRequested{Generation{1}}, 0);
    state = step(loading, SourceFailed{Generation{1}, LoadAttempt{1}}, .04);
    CHECK(state.failure == FailureReason::SourceUnavailable);
    CHECK(state.attempt == 0);
}

TEST_CASE("all stale edges and duplicate requests are discarded") {
    const auto state = step(reach_steady(), ChannelRequested{Generation{2}}, 7);
    const std::array<SupervisorEvent, 12> stale{
        StreamEnded{Generation{1}, LoadAttempt{1}, EndReason::Error, {}}, FirstFrame{Generation{1}, LoadAttempt{1}},
        ProcessExited{Generation{1}, LoadAttempt{1}}, IpcUnresponsive{Generation{1}, LoadAttempt{1}},
        AuthRejected{Generation{1}, LoadAttempt{1}}, CacheState{Generation{1}, LoadAttempt{1}, true},
        PlaybackHealthObserved{Generation{1}, LoadAttempt{1}, true},
        StreamLoadIssued{Generation{1}, LoadAttempt{1}, LoadIntent::FreshSelection, RecoveryTransport::MpegTs},
        SourceFailed{Generation{1}, LoadAttempt{1}}, PlaybackStalled{Generation{1}, LoadAttempt{1}, StallKind::Progress},
        DecodeStalled{Generation{1}, LoadAttempt{1}},
        ForwardProgressObserved{Generation{1}, LoadAttempt{1}}};
    for (const auto& event : stale) {
        const auto result = apply(state, event, 8);
        CHECK_FALSE(result.transition);
        CHECK(result.effects.empty());
        CHECK(result.state.name == SupervisorStateName::Loading);
    }
    CHECK_FALSE(apply(state, ChannelRequested{Generation{2}}, 9).transition);
    CHECK_FALSE(apply(state, ChannelRequested{Generation{1}}, 9).transition);
}

TEST_CASE("same-generation events from a replaced load attempt are ignored") {
    auto state = step(reach_steady(), StreamEnded{
        Generation{1}, LoadAttempt{1}, EndReason::Eof, {}}, 10);
    state = step(state, DeadlineReached{}, 10.5);
    state = step(state, StreamLoadIssued{
        Generation{1}, LoadAttempt{2}, LoadIntent::RecoveryReopen,
        RecoveryTransport::MpegTs}, 10.51);

    const std::array<SupervisorEvent, 6> stale{
        FirstFrame{Generation{1}, LoadAttempt{1}},
        StreamEnded{Generation{1}, LoadAttempt{1}, EndReason::Eof, {}},
        PlaybackStalled{Generation{1}, LoadAttempt{1}, StallKind::Progress},
        DecodeStalled{Generation{1}, LoadAttempt{1}},
        CacheState{Generation{1}, LoadAttempt{1}, true},
        ForwardProgressObserved{Generation{1}, LoadAttempt{1}}};
    for (const auto& event : stale) {
        const auto result = apply(state, event, 11);
        CHECK_FALSE(result.transition);
        CHECK(result.effects.empty());
        CHECK(result.state.name == SupervisorStateName::Zap);
        CHECK(result.state.load_attempt == LoadAttempt{2});
    }
}

TEST_CASE("a newer request disarms recovery and late deadlines") {
    auto state = step(reach_steady(), StreamEnded{Generation{1}, LoadAttempt{1}, EndReason::Error, {}}, 10);
    CHECK(next_deadline_at(state) == at(10.5));
    state = step(state, ChannelRequested{Generation{2}}, 10.1);
    CHECK(state.name == SupervisorStateName::Loading);
    CHECK(state.attempt == 0);
    CHECK_FALSE(next_deadline_at(state));
    CHECK_FALSE(apply(state, DeadlineReached{}, 10.5).transition);
}

TEST_CASE("cache levels do not recover and explicit stop can advance to idle") {
    auto result = apply(reach_steady(), CacheState{Generation{1}, LoadAttempt{1}, true}, 10);
    CHECK(result.transition);
    CHECK(result.state.cache_paused);
    CHECK(result.state.name == SupervisorStateName::Steady);
    CHECK(result.effects.empty());
    result = apply(result.state, CacheState{Generation{1}, LoadAttempt{1}, true}, 10.1);
    CHECK(result.transition);

    const auto stopped = step(result.state, PlaybackStopped{Generation{2}}, 11);
    CHECK(stopped.name == SupervisorStateName::Idle);
    CHECK(stopped.generation == Generation{2});
}

TEST_CASE("transitions and diagnostics carry generation transport budget and policy") {
    auto state = step(reach_steady(), ForwardProgressObserved{
        Generation{1}, LoadAttempt{1}}, 9);
    auto result = apply(state, StreamEnded{
        Generation{1}, LoadAttempt{1}, EndReason::Eof, {}}, 10);
    REQUIRE(result.transition);
    CHECK(result.transition->attempt == 1);
    CHECK(result.transition->elapsed_budget == Duration::zero());
    CHECK(result.transition->from == SupervisorStateName::Steady);
    CHECK(result.transition->reason == "stream-ended-eof");
    CHECK(result.transition->transport == RecoveryTransport::MpegTs);
    CHECK(result.transition->transport_policy_version == kTransportPolicyVersion);
    CHECK(result.transition->load_attempt == LoadAttempt{1});
    CHECK(result.transition->load_intent == LoadIntent::FreshSelection);
    CHECK(result.transition->outcome == RecoveryOutcome::FaultDecided);
    CHECK(result.transition->last_progress_to_decision == seconds(1));

    const auto stats = project_supervisor_stats(result.state, at(10.4));
    CHECK(stats.attempt == 1);
    CHECK(stats.attempt_ceiling == 5);
    REQUIRE(stats.elapsed_budget);
    CHECK(stats.elapsed_budget->count() == Approx(0.4));
    CHECK(stats.reason == "stream-ended-eof");

    const auto idle_stats = project_supervisor_stats(initial_supervisor_state(), at(5));
    CHECK_FALSE(idle_stats.elapsed_budget);
    CHECK_FALSE(idle_stats.reason);
}

TEST_CASE("recovery timing phases stay attached to their load attempt") {
    auto state = step(reach_steady(), ForwardProgressObserved{
        Generation{1}, LoadAttempt{1}}, 9);
    state = step(state, StreamEnded{
        Generation{1}, LoadAttempt{1}, EndReason::Eof, {}}, 10);
    state = step(state, DeadlineReached{}, 10.5);
    auto issued = apply(state, StreamLoadIssued{
        Generation{1}, LoadAttempt{2}, LoadIntent::RecoveryReopen,
        RecoveryTransport::MpegTs}, 10.6);
    REQUIRE(issued.transition);
    CHECK(issued.transition->load_attempt == LoadAttempt{2});
    CHECK(issued.transition->load_intent == LoadIntent::RecoveryReopen);
    REQUIRE(issued.transition->decision_to_command);
    CHECK(issued.transition->decision_to_command->count() == Approx(.6));

    auto first = apply(issued.state, FirstFrame{
        Generation{1}, LoadAttempt{2}}, 12.1);
    REQUIRE(first.transition);
    CHECK(first.transition->load_attempt == LoadAttempt{2});
    REQUIRE(first.transition->command_to_first_frame);
    CHECK(first.transition->command_to_first_frame->count() == Approx(1.5));
    CHECK(first.transition->outcome == RecoveryOutcome::FirstFrame);

    state = step(first.state, PlaybackHealthObserved{
        Generation{1}, LoadAttempt{2}, false}, 12.1);
    auto clean = apply(state, DeadlineReached{}, 17.1);
    REQUIRE(clean.transition);
    CHECK(clean.transition->outcome == RecoveryOutcome::CleanProbation);
    REQUIRE(clean.transition->first_frame_to_outcome);
    CHECK(clean.transition->first_frame_to_outcome->count() == Approx(5));
    REQUIRE(clean.transition->recovered_load_lifetime);
    CHECK(clean.transition->recovered_load_lifetime->count() == Approx(6.5));
}

TEST_CASE("steady window follows the current playback health level") {
    auto state = step(initial_supervisor_state(), ChannelRequested{Generation{1}}, 0);
    state = step(state, StreamLoadIssued{Generation{1}, LoadAttempt{1}, LoadIntent::FreshSelection, RecoveryTransport::MpegTs}, .01);
    state = step(state, FirstFrame{Generation{1}, LoadAttempt{1}}, 1);
    state = step(state, PlaybackHealthObserved{Generation{1}, LoadAttempt{1}, false}, 1);
    CHECK(next_deadline_at(state) == at(6));

    auto result = apply(state, PlaybackHealthObserved{Generation{1}, LoadAttempt{1}, true}, 4);
    CHECK(result.transition->reason == "playback-unhealthy-restarted-steady-window");
    CHECK(next_deadline_at(result.state) == at(9));

    // A persistent unhealthy verdict has no further edge. The deadline must
    // consult the level or it would confirm here anyway.
    result = apply(result.state, DeadlineReached{}, 9);
    CHECK(result.state.name == SupervisorStateName::Zap);
    CHECK(result.transition->reason == "steady-window-held-by-unhealthy-playback");
    CHECK(next_deadline_at(result.state) == at(14));

    // The full clean interval starts at the healthy edge, not at the held
    // deadline or at the first frame.
    result = apply(result.state, PlaybackHealthObserved{Generation{1}, LoadAttempt{1}, false}, 10);
    CHECK(result.transition->reason == "playback-health-restarted-steady-window");
    CHECK(next_deadline_at(result.state) == at(15));
    CHECK_FALSE(apply(result.state, DeadlineReached{}, 14).transition);
    state = step(result.state, DeadlineReached{}, 15);
    CHECK(state.name == SupervisorStateName::Steady);

    // Outside Zap the level is still recorded, but there is no window to arm.
    result = apply(state, PlaybackHealthObserved{Generation{1}, LoadAttempt{1}, true}, 16);
    CHECK(result.state.name == SupervisorStateName::Steady);
    CHECK(result.state.playback_unhealthy);
    CHECK_FALSE(next_deadline_at(result.state));
}

TEST_CASE("sustained degradation cannot clear a recovery attempt before stall recovery") {
    auto state = step(reach_steady(), StreamEnded{Generation{1}, LoadAttempt{1}, EndReason::Error, {}}, 10);
    REQUIRE(state.attempt == 1);
    state = step(state, DeadlineReached{}, 10.5);
    state = step(state, StreamLoadIssued{Generation{1}, LoadAttempt{2}, LoadIntent::RecoveryReopen, RecoveryTransport::MpegTs}, 10.51);
    state = step(state, FirstFrame{Generation{1}, LoadAttempt{2}}, 11);

    // The recovered load remains degraded. There is no later interruption
    // edge, but its attempt must not be laundered at the steady deadline.
    state = step(state, PlaybackHealthObserved{Generation{1}, LoadAttempt{2}, true}, 11.5);
    auto held = apply(state, DeadlineReached{}, 16.5);
    CHECK(held.state.name == SupervisorStateName::Zap);
    CHECK(held.state.attempt == 1);
    CHECK(held.transition->reason == "steady-window-held-by-unhealthy-playback");

    // When the decode-stall threshold fires, this is attempt two in the same
    // recovery episode, rather than a fresh attempt one.
    state = step(held.state, DecodeStalled{Generation{1}, LoadAttempt{2}}, 17.5);
    CHECK(state.name == SupervisorStateName::Recovering);
    CHECK(state.attempt == 2);
    CHECK(state.detection == DetectionReason::DecodeStall);
}

TEST_CASE("steady is not confirmed while the cache is still holding playback back") {
    auto state = step(initial_supervisor_state(), ChannelRequested{Generation{1}}, 0);
    state = step(state, StreamLoadIssued{Generation{1}, LoadAttempt{1}, LoadIntent::FreshSelection, RecoveryTransport::MpegTs}, .01);

    // The opening fill starts before anything is shown, so the health fold has
    // already classified the load degraded by the time a frame arrives. There
    // is no healthy-to-degraded transition left to announce the condition.
    state = step(state, CacheState{Generation{1}, LoadAttempt{1}, true}, 1);
    state = step(state, FirstFrame{Generation{1}, LoadAttempt{1}}, 2);
    CHECK(next_deadline_at(state) == at(7));

    // The fill outlasts the window. Confirming here would mean "five seconds
    // since a first frame", not "five seconds of playback".
    auto result = apply(state, DeadlineReached{}, 7);
    CHECK(result.state.name == SupervisorStateName::Zap);
    CHECK(result.transition->reason == "steady-window-held-by-cache-pause");
    CHECK(next_deadline_at(result.state) == at(12));

    // It holds for as long as the fill does.
    result = apply(result.state, DeadlineReached{}, 12);
    CHECK(result.state.name == SupervisorStateName::Zap);
    CHECK(next_deadline_at(result.state) == at(17));

    // Playback finally starts, and the five seconds are counted from there.
    // Letting the held deadline stand would confirm on four clean seconds here,
    // and on almost none at all if the fill cleared just before it expired.
    auto resumed = apply(result.state, CacheState{Generation{1}, LoadAttempt{1}, false}, 13);
    CHECK(resumed.transition->reason == "cache-resume-restarted-steady-window");
    CHECK(next_deadline_at(resumed.state) == at(18));

    state = step(resumed.state, PlaybackHealthObserved{Generation{1}, LoadAttempt{1}, false}, 13);
    CHECK_FALSE(apply(state, DeadlineReached{}, 17).transition);
    state = step(state, DeadlineReached{}, 18);
    CHECK(state.name == SupervisorStateName::Steady);
}

TEST_CASE("a fill clearing just before the deadline still buys a full clean window") {
    auto state = step(initial_supervisor_state(), ChannelRequested{Generation{1}}, 0);
    state = step(state, StreamLoadIssued{Generation{1}, LoadAttempt{1}, LoadIntent::FreshSelection, RecoveryTransport::MpegTs}, .01);
    state = step(state, FirstFrame{Generation{1}, LoadAttempt{1}}, 1);
    CHECK(next_deadline_at(state) == at(6));

    // The whole window is spent filling, and the fill clears with 100ms to go.
    state = step(state, CacheState{Generation{1}, LoadAttempt{1}, true}, 1.5);
    state = step(state, CacheState{Generation{1}, LoadAttempt{1}, false}, 5.9);
    state = step(state, PlaybackHealthObserved{Generation{1}, LoadAttempt{1}, false}, 5.9);
    CHECK(next_deadline_at(state) == at(10.9));

    // Nothing has played cleanly for five seconds yet, so nothing is confirmed.
    CHECK_FALSE(apply(state, DeadlineReached{}, 6).transition);
    state = step(state, DeadlineReached{}, 10.9);
    CHECK(state.name == SupervisorStateName::Steady);
}

TEST_CASE("entering the cache pause restarts the steady window") {
    auto state = step(initial_supervisor_state(), ChannelRequested{Generation{1}}, 0);
    state = step(state, StreamLoadIssued{Generation{1}, LoadAttempt{1}, LoadIntent::FreshSelection, RecoveryTransport::MpegTs}, .01);
    state = step(state, FirstFrame{Generation{1}, LoadAttempt{1}}, 1);
    state = step(state, PlaybackHealthObserved{Generation{1}, LoadAttempt{1}, false}, 1);
    CHECK(next_deadline_at(state) == at(6));

    // A fill entered after the window is armed interrupts the evidence it is
    // counting, whether or not the fold called it a new degradation edge.
    auto result = apply(state, CacheState{Generation{1}, LoadAttempt{1}, true}, 3);
    CHECK(result.transition->reason == "cache-pause-restarted-steady-window");
    CHECK(next_deadline_at(result.state) == at(8));

    // Leaving it restarts the window too: clean playback starts here, and the
    // three seconds before the fill are not credit towards it.
    state = step(result.state, CacheState{Generation{1}, LoadAttempt{1}, false}, 4);
    CHECK(next_deadline_at(state) == at(9));

    // A repeated observation of playing is not an edge. Restarting on it would
    // mean a load that keeps reporting good news never confirms.
    const auto repeated = apply(state, CacheState{Generation{1}, LoadAttempt{1}, false}, 6);
    CHECK(repeated.transition->reason == "cache-state-observed");
    CHECK(next_deadline_at(repeated.state) == at(9));

    state = step(state, DeadlineReached{}, 9);
    CHECK(state.name == SupervisorStateName::Steady);

    // Outside Zap it is only a cache observation: Steady has no window to hold.
    const auto settled = apply(state, CacheState{Generation{1}, LoadAttempt{1}, true}, 10);
    CHECK(settled.transition->reason == "cache-state-observed");
    CHECK(settled.state.name == SupervisorStateName::Steady);
}

TEST_CASE("cache progress and decode stalls use transport branch and shared budget") {
    auto state = step(reach_steady(),
                      PlaybackStalled{Generation{1}, LoadAttempt{1}, StallKind::Cache}, 20);
    CHECK(state.detection == DetectionReason::CacheStall);
    CHECK(recovery(state) == RecoveryAction::ReopenStream);
    CHECK(state.attempt == 1);
    CHECK(next_deadline_at(state) == at(20.5));
    CHECK(to_string(*state.detection) == std::string_view{"cache-stall"});

    state = step(reach_steady(), PlaybackStalled{Generation{1}, LoadAttempt{1}, StallKind::Progress}, 20);
    CHECK(state.detection == DetectionReason::ProgressStall);
    CHECK(recovery(state) == RecoveryAction::ReopenStream);
    CHECK(state.attempt == 1);
    CHECK(next_deadline_at(state) == at(20.5));

    state = step(reach_steady(), DecodeStalled{Generation{1}, LoadAttempt{1}}, 20);
    CHECK(state.detection == DetectionReason::DecodeStall);
    CHECK(recovery(state) == RecoveryAction::ReopenStream);
    state = step(reach_steady(Generation{1}, RecoveryTransport::Hls),
                 DecodeStalled{Generation{1}, LoadAttempt{1}}, 20);
    CHECK(recovery(state) == RecoveryAction::ReloadHlsLive);
}

TEST_CASE("format probe failure emits its distinct bounded reopen") {
    auto state = step(initial_supervisor_state(), ChannelRequested{Generation{1}}, 0);
    state = step(state, StreamLoadIssued{Generation{1}, LoadAttempt{1}, LoadIntent::FreshSelection, RecoveryTransport::MpegTs}, .01);
    state = step(state, StreamEnded{Generation{1}, LoadAttempt{1}, EndReason::Error,
                                   TransportFailureReason::FormatProbeRequired}, .5);
    CHECK(state.detection == DetectionReason::FormatProbeRequired);
    CHECK(recovery(state) == RecoveryAction::ReopenProbed);
    CHECK(state.attempt == 1);
    const auto result = apply(state, DeadlineReached{}, 1.1);
    REQUIRE(result.effects.size() == 1);
    CHECK(std::holds_alternative<ReopenProbed>(result.effects[0].payload));
}

TEST_CASE("deadline derivation follows the deadline owned by each valid state") {
    auto state = initial_supervisor_state();
    state.name = SupervisorStateName::Recovering;
    state.deadlines.retry_at = at(.9);
    CHECK(next_deadline_at(state) == at(.9));
    state.name = SupervisorStateName::Zap;
    state.deadlines.retry_at.reset();
    state.deadlines.steady_at = at(.4);
    CHECK(next_deadline_at(state) == at(.4));
    state.deadlines.steady_at.reset();
    CHECK_FALSE(next_deadline_at(state));
}

TEST_CASE("reachable supervisor states keep deadline kinds paired with their owner") {
    auto state = initial_supervisor_state();
    CHECK(supervisor_deadlines_valid(state));
    auto step = [&](const SupervisorEvent& event, double now) {
        state = reduce_supervisor_state(state, event, TimePoint{seconds(now)}).state;
        CHECK(supervisor_deadlines_valid(state));
    };

    step(ChannelRequested{Generation{1}}, 0.0);
    step(StreamLoadIssued{Generation{1}, LoadAttempt{1}, LoadIntent::FreshSelection, RecoveryTransport::MpegTs}, 0.0);
    step(FirstFrame{Generation{1}, LoadAttempt{1}}, 0.1);
    step(PlaybackHealthObserved{Generation{1}, LoadAttempt{1}, false}, 0.1);
    step(DeadlineReached{}, 5.1);
    step(StreamEnded{Generation{1}, LoadAttempt{1}, EndReason::Error, {}}, 6.0);
    step(DeadlineReached{}, 6.5);
    step(StreamLoadIssued{Generation{1}, LoadAttempt{2}, LoadIntent::RecoveryReopen, RecoveryTransport::MpegTs}, 6.5);

    auto invalid = state;
    invalid.deadlines.retry_at = TimePoint{};
    CHECK_FALSE(supervisor_deadlines_valid(invalid));
    invalid = state;
    invalid.deadlines.steady_at = TimePoint{};
    invalid.deadlines.retry_at = TimePoint{};
    CHECK_FALSE(supervisor_deadlines_valid(invalid));
}
