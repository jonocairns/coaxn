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
    state = step(state, StreamLoadIssued{generation, transport}, 0.01);
    state = step(state, FirstFrame{generation}, 1.0);
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
    CHECK(kDefaultRecoveryPolicy.version == "coax-recovery-v1");
    CHECK(kTransportPolicyVersion == "coax-transport-recovery-v4");
}

TEST_CASE("idle owns no stream and clean playback reaches steady after five seconds") {
    const auto idle = initial_supervisor_state();
    CHECK_FALSE(apply(idle, ProcessExited{Generation{1}}, 1).transition);
    CHECK_FALSE(apply(idle, IpcUnresponsive{Generation{1}}, 1).transition);
    CHECK_FALSE(apply(idle, StreamEnded{Generation{1}, EndReason::Error, {}}, 1).transition);

    auto state = step(idle, ChannelRequested{Generation{1}}, 0);
    CHECK(state.name == SupervisorStateName::Loading);
    CHECK_FALSE(state.transport);
    state = step(state, StreamLoadIssued{Generation{1}, RecoveryTransport::MpegTs}, .01);
    CHECK(state.name == SupervisorStateName::Zap);
    state = step(state, FirstFrame{Generation{1}}, 1);
    CHECK(next_deadline_at(state) == at(6));
    CHECK_FALSE(apply(state, DeadlineReached{}, 5.999).transition);
    state = step(state, DeadlineReached{}, 6);
    CHECK(state.name == SupervisorStateName::Steady);
    CHECK_FALSE(next_deadline_at(state));
}

TEST_CASE("continuous TS terminal end schedules and emits a generation-scoped reopen") {
    auto result = apply(reach_steady(), StreamEnded{Generation{1}, EndReason::Error, {}}, 10);
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

TEST_CASE("attempt budget resets only after recovered playback is steady") {
    auto state = step(reach_steady(), StreamEnded{Generation{1}, EndReason::Error, {}}, 10);
    state = step(state, DeadlineReached{}, 10.5);
    state = step(state, StreamLoadIssued{Generation{1}, RecoveryTransport::MpegTs}, 10.51);
    state = step(state, FirstFrame{Generation{1}}, 11.2);
    CHECK(state.attempt == 1);
    state = step(state, DeadlineReached{}, 16.2);
    CHECK(state.name == SupervisorStateName::Steady);
    CHECK(state.attempt == 0);
    CHECK_FALSE(state.recovery_started_at);
    CHECK_FALSE(state.detection);
}

TEST_CASE("backend failures map to in-process player recreation for both transports") {
    auto state = step(reach_steady(), ProcessExited{Generation{1}}, 10);
    CHECK(recovery(state) == RecoveryAction::RecreatePlayer);
    auto fired = apply(state, DeadlineReached{}, 10.5);
    REQUIRE(fired.effects.size() == 1);
    CHECK(std::holds_alternative<RecreatePlayer>(fired.effects[0].payload));

    state = step(reach_steady(Generation{1}, RecoveryTransport::Hls),
                 IpcUnresponsive{Generation{1}}, 10);
    CHECK(recovery(state) == RecoveryAction::RecreatePlayer);
    CHECK(state.detection == DetectionReason::IpcUnresponsive);
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
                            StreamEnded{Generation{1}, EndReason::Error, reason}, 10);
        CHECK(recovery(result.state) == RecoveryAction::ReloadHlsLive);
        auto fired = apply(result.state, DeadlineReached{}, 10.5);
        REQUIRE(fired.effects.size() == 1);
        CHECK(std::holds_alternative<ReloadHlsLive>(fired.effects[0].payload));
    }
    auto state = step(reach_steady(Generation{1}, RecoveryTransport::Hls),
                      PlaybackStalled{Generation{1}, StallKind::Open}, 20);
    CHECK(state.detection == DetectionReason::OpenStall);
    CHECK(recovery(state) == RecoveryAction::ReloadHlsLive);
}

TEST_CASE("five attempts share one episode then terminate") {
    auto state = reach_steady();
    double now = 10.0;
    std::array<double, 5> delays{};
    for (std::size_t attempt = 0; attempt < delays.size(); ++attempt) {
        state = step(state, StreamEnded{Generation{1}, EndReason::Error, {}}, now);
        CHECK(state.attempt == attempt + 1);
        REQUIRE(next_deadline_at(state));
        delays[attempt] = (next_deadline_at(state).value() - at(now)).count();
        now += delays[attempt];
        state = step(state, DeadlineReached{}, now);
    }
    CHECK(delays == std::array<double, 5>{0.5, 1.0, 2.0, 4.0, 5.0});
    const auto result = apply(state, StreamEnded{Generation{1}, EndReason::Error, {}}, now);
    CHECK(result.state.name == SupervisorStateName::Failed);
    CHECK(result.state.failure == FailureReason::AttemptsExhausted);
    CHECK(result.effects.empty());
}

TEST_CASE("an attempt outside the thirty second budget is not started") {
    auto state = step(reach_steady(), StreamEnded{Generation{1}, EndReason::Error, {}}, 0);
    CHECK(state.recovery_started_at == at(0));
    const auto result = apply(state, StreamEnded{Generation{1}, EndReason::Error, {}}, 29.8);
    CHECK(result.state.name == SupervisorStateName::Failed);
    CHECK(result.state.failure == FailureReason::BudgetExpired);
    CHECK(result.state.attempt == 1);
}

TEST_CASE("terminal auth and source failures wait for a newer generation") {
    auto state = step(reach_steady(), AuthRejected{Generation{1}}, 10);
    CHECK(state.name == SupervisorStateName::Failed);
    CHECK(state.failure == FailureReason::AuthRejected);
    CHECK_FALSE(apply(state, ProcessExited{Generation{1}}, 12).transition);
    CHECK_FALSE(apply(state, DeadlineReached{}, 12).transition);
    state = step(state, ChannelRequested{Generation{2}}, 13);
    CHECK(state.name == SupervisorStateName::Loading);
    CHECK_FALSE(state.failure);

    auto loading = step(initial_supervisor_state(), ChannelRequested{Generation{1}}, 0);
    state = step(loading, SourceFailed{Generation{1}}, .04);
    CHECK(state.failure == FailureReason::SourceUnavailable);
    CHECK(state.attempt == 0);
}

TEST_CASE("all stale edges and duplicate requests are discarded") {
    const auto state = step(reach_steady(), ChannelRequested{Generation{2}}, 7);
    const std::array<SupervisorEvent, 10> stale{
        StreamEnded{Generation{1}, EndReason::Error, {}}, FirstFrame{Generation{1}},
        ProcessExited{Generation{1}}, IpcUnresponsive{Generation{1}},
        AuthRejected{Generation{1}}, CacheState{Generation{1}, true},
        StreamLoadIssued{Generation{1}, RecoveryTransport::MpegTs}, SourceFailed{Generation{1}},
        PlaybackStalled{Generation{1}, StallKind::Progress}, DecodeStalled{Generation{1}}};
    for (const auto& event : stale) {
        const auto result = apply(state, event, 8);
        CHECK_FALSE(result.transition);
        CHECK(result.effects.empty());
        CHECK(result.state.name == SupervisorStateName::Loading);
    }
    CHECK_FALSE(apply(state, ChannelRequested{Generation{2}}, 9).transition);
    CHECK_FALSE(apply(state, ChannelRequested{Generation{1}}, 9).transition);
}

TEST_CASE("a newer request disarms recovery and late deadlines") {
    auto state = step(reach_steady(), StreamEnded{Generation{1}, EndReason::Error, {}}, 10);
    CHECK(next_deadline_at(state) == at(10.5));
    state = step(state, ChannelRequested{Generation{2}}, 10.1);
    CHECK(state.name == SupervisorStateName::Loading);
    CHECK(state.attempt == 0);
    CHECK_FALSE(next_deadline_at(state));
    CHECK_FALSE(apply(state, DeadlineReached{}, 10.5).transition);
}

TEST_CASE("cache levels do not recover and explicit stop can advance to idle") {
    auto result = apply(reach_steady(), CacheState{Generation{1}, true}, 10);
    CHECK(result.transition);
    CHECK(result.state.cache_paused);
    CHECK(result.state.name == SupervisorStateName::Steady);
    CHECK(result.effects.empty());
    result = apply(result.state, CacheState{Generation{1}, true}, 10.1);
    CHECK(result.transition);

    const auto stopped = step(result.state, PlaybackStopped{Generation{2}}, 11);
    CHECK(stopped.name == SupervisorStateName::Idle);
    CHECK(stopped.generation == Generation{2});
}

TEST_CASE("transitions and diagnostics carry generation transport budget and policy") {
    auto result = apply(reach_steady(), StreamEnded{Generation{1}, EndReason::Eof, {}}, 10);
    REQUIRE(result.transition);
    CHECK(result.transition->attempt == 1);
    CHECK(result.transition->elapsed_budget == Duration::zero());
    CHECK(result.transition->from == SupervisorStateName::Steady);
    CHECK(result.transition->reason == "stream-ended-eof");
    CHECK(result.transition->transport == RecoveryTransport::MpegTs);
    CHECK(result.transition->transport_policy_version == kTransportPolicyVersion);

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

TEST_CASE("healthy steady window restarts only while armed and phase changes once") {
    auto state = step(initial_supervisor_state(), ChannelRequested{Generation{1}}, 0);
    state = step(state, StreamLoadIssued{Generation{1}, RecoveryTransport::MpegTs}, .01);
    CHECK_FALSE(apply(state, PlaybackInterrupted{Generation{1}}, 2).transition);
    state = step(state, FirstFrame{Generation{1}}, 1);
    CHECK(next_deadline_at(state) == at(6));
    auto result = apply(state, PlaybackInterrupted{Generation{1}}, 4);
    CHECK(result.transition->reason == "steady-window-restarted");
    CHECK(next_deadline_at(result.state) == at(9));
    CHECK_FALSE(apply(result.state, DeadlineReached{}, 6).transition);
    state = step(result.state, DeadlineReached{}, 9);
    CHECK(state.name == SupervisorStateName::Steady);
    CHECK_FALSE(apply(state, PlaybackInterrupted{Generation{1}}, 12).transition);
}

TEST_CASE("progress and decode stalls use transport branch and shared budget") {
    auto state = step(reach_steady(), PlaybackStalled{Generation{1}, StallKind::Progress}, 20);
    CHECK(state.detection == DetectionReason::ProgressStall);
    CHECK(recovery(state) == RecoveryAction::ReopenStream);
    CHECK(state.attempt == 1);
    CHECK(next_deadline_at(state) == at(20.5));

    state = step(reach_steady(), DecodeStalled{Generation{1}}, 20);
    CHECK(state.detection == DetectionReason::DecodeStall);
    CHECK(recovery(state) == RecoveryAction::ReopenStream);
    state = step(reach_steady(Generation{1}, RecoveryTransport::Hls),
                 DecodeStalled{Generation{1}}, 20);
    CHECK(recovery(state) == RecoveryAction::ReloadHlsLive);
}

TEST_CASE("format probe failure emits its distinct bounded reopen") {
    auto state = step(initial_supervisor_state(), ChannelRequested{Generation{1}}, 0);
    state = step(state, StreamLoadIssued{Generation{1}, RecoveryTransport::MpegTs}, .01);
    state = step(state, StreamEnded{Generation{1}, EndReason::Error,
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
    step(StreamLoadIssued{Generation{1}, RecoveryTransport::MpegTs}, 0.0);
    step(FirstFrame{Generation{1}}, 0.1);
    step(DeadlineReached{}, 5.1);
    step(StreamEnded{Generation{1}, EndReason::Error, {}}, 6.0);
    step(DeadlineReached{}, 6.5);
    step(StreamLoadIssued{Generation{1}, RecoveryTransport::MpegTs}, 6.5);

    auto invalid = state;
    invalid.deadlines.retry_at = TimePoint{};
    CHECK_FALSE(supervisor_deadlines_valid(invalid));
    invalid = state;
    invalid.deadlines.steady_at = TimePoint{};
    invalid.deadlines.retry_at = TimePoint{};
    CHECK_FALSE(supervisor_deadlines_valid(invalid));
}
