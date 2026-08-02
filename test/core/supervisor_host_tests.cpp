#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "core/supervisor_host.hpp"

using namespace coax::core;

namespace {
class FakeClock final : public SupervisorClock {
public:
    TimePoint current{};
    [[nodiscard]] TimePoint now() const override { return current; }
    void advance_to(double seconds_value) { current = TimePoint{seconds(seconds_value)}; }
};

struct HostFixture {
    FakeClock clock;
    std::vector<SupervisorEffect> effects;
    std::vector<SupervisorTransition> transitions;
    PlaybackSupervisor supervisor;

    HostFixture()
        : supervisor(clock, {
              .on_effect = [this](const auto& effect) { effects.push_back(effect); },
              .on_state_changed = {},
              .on_transition = [this](const auto& transition) {
                  transitions.push_back(transition);
              }}) {}

    void start(Generation generation = Generation{1}) {
        supervisor.dispatch(ChannelRequested{generation});
        supervisor.dispatch(StreamLoadIssued{generation, RecoveryTransport::MpegTs});
    }
    void advance(double seconds_value) { clock.advance_to(seconds_value); supervisor.poll(); }
};
}  // namespace

TEST_CASE("host owns one deadline and runs effects when it expires") {
    HostFixture host;
    host.start();
    CHECK_FALSE(host.supervisor.armed_deadline());
    host.supervisor.dispatch(FirstFrame{Generation{1}});
    CHECK(host.supervisor.armed_deadline() == TimePoint{seconds(5)});
    host.advance(5);
    CHECK(host.supervisor.current().name == SupervisorStateName::Steady);
    CHECK_FALSE(host.supervisor.armed_deadline());

    host.supervisor.dispatch(StreamEnded{Generation{1}, EndReason::Error, {}});
    CHECK(host.supervisor.armed_deadline() == TimePoint{seconds(5.5)});
    host.advance(5.5);
    REQUIRE(host.effects.size() == 1);
    CHECK(std::holds_alternative<ReopenStream>(host.effects[0].payload));
    CHECK_FALSE(host.supervisor.armed_deadline());
}

TEST_CASE("synchronous effect settlement is queued instead of reentering callbacks") {
    FakeClock clock;
    std::vector<SupervisorStateName> states;
    std::vector<std::string> reasons;
    PlaybackSupervisor* host = nullptr;
    PlaybackSupervisor supervisor(
        clock,
        {.on_effect = [&](const SupervisorEffect& effect) {
             host->dispatch(StreamLoadIssued{effect.generation,
                                             RecoveryTransport::MpegTs});
         },
         .on_state_changed = [&](const SupervisorState& state) {
             states.push_back(state.name);
         },
         .on_transition = [&](const SupervisorTransition& transition) {
             reasons.push_back(transition.reason);
         }});
    host = &supervisor;

    supervisor.dispatch(ChannelRequested{Generation{1}});
    supervisor.dispatch(StreamLoadIssued{Generation{1}, RecoveryTransport::MpegTs});
    supervisor.dispatch(FirstFrame{Generation{1}});
    clock.advance_to(5.0);
    supervisor.poll();
    supervisor.dispatch(StreamEnded{Generation{1}, EndReason::Error, {}});
    states.clear();
    reasons.clear();

    clock.advance_to(5.5);
    supervisor.poll();

    CHECK(states == std::vector<SupervisorStateName>{SupervisorStateName::Recovering,
                                                     SupervisorStateName::Zap});
    CHECK(reasons == std::vector<std::string>{"recovery-attempt-started",
                                              "stream-load-issued"});
    CHECK(supervisor.current().name == SupervisorStateName::Zap);
}

TEST_CASE("backend recreation is bounded and a rejected attempt terminates") {
    HostFixture host;
    host.start(); host.supervisor.dispatch(FirstFrame{Generation{1}}); host.advance(5);
    host.supervisor.dispatch(ProcessExited{Generation{1}});
    CHECK(host.supervisor.armed_deadline() == TimePoint{seconds(5.5)});
    host.advance(5.5);
    REQUIRE(host.effects.size() == 1);
    CHECK(std::holds_alternative<RecreatePlayer>(host.effects[0].payload));
    CHECK(host.supervisor.current().name == SupervisorStateName::Recovering);
    host.supervisor.dispatch(SourceFailed{Generation{1}});
    CHECK(host.supervisor.current().failure == FailureReason::SourceUnavailable);
    CHECK_FALSE(host.supervisor.armed_deadline());
}

TEST_CASE("recreation attempts share budget until steady then reset") {
    HostFixture host;
    host.start(); host.supervisor.dispatch(FirstFrame{Generation{1}}); host.advance(5);
    host.supervisor.dispatch(ProcessExited{Generation{1}}); host.advance(5.5);
    host.supervisor.dispatch(StreamLoadIssued{Generation{1}, RecoveryTransport::MpegTs});
    host.supervisor.dispatch(ProcessExited{Generation{1}});
    CHECK(host.supervisor.current().attempt == 2);
    CHECK(host.supervisor.armed_deadline() == TimePoint{seconds(6.5)});
    host.advance(6.5);
    CHECK(host.effects.size() == 2);

    host.supervisor.dispatch(StreamLoadIssued{Generation{1}, RecoveryTransport::MpegTs});
    host.supervisor.dispatch(FirstFrame{Generation{1}});
    host.advance(11.5);
    CHECK(host.supervisor.current().name == SupervisorStateName::Steady);
    host.supervisor.dispatch(ProcessExited{Generation{1}});
    CHECK(host.supervisor.current().attempt == 1);
}

TEST_CASE("new generation and disposal disarm the host") {
    HostFixture host;
    host.start(); host.supervisor.dispatch(FirstFrame{Generation{1}}); host.advance(5);
    host.supervisor.dispatch(StreamEnded{Generation{1}, EndReason::Error, {}});
    REQUIRE(host.supervisor.armed_deadline());
    host.supervisor.dispatch(ChannelRequested{Generation{2}});
    CHECK_FALSE(host.supervisor.armed_deadline());
    host.advance(10);
    CHECK(host.effects.empty());
    CHECK(host.supervisor.current().name == SupervisorStateName::Loading);
    host.supervisor.dispose();
    host.supervisor.dispatch(StreamLoadIssued{Generation{2}, RecoveryTransport::MpegTs});
    CHECK(host.supervisor.current().name == SupervisorStateName::Loading);
}

TEST_CASE("host reports accepted transitions and ignores stale events") {
    HostFixture host;
    host.start();
    REQUIRE(host.transitions.size() == 2);
    host.supervisor.dispatch(FirstFrame{Generation{99}});
    CHECK(host.transitions.size() == 2);
    host.supervisor.dispatch(FirstFrame{Generation{1}}); host.advance(5);
    host.supervisor.dispatch(StreamEnded{Generation{1}, EndReason::Eof, {}});
    REQUIRE_FALSE(host.transitions.empty());
    const auto& transition = host.transitions.back();
    CHECK(transition.reason == "stream-ended-eof");
    CHECK(transition.policy_version == "coax-recovery-v1");
    CHECK(transition.transport_policy_version == "coax-transport-recovery-v4");
}

TEST_CASE("repeated backend failure exhausts the shared recreation schedule") {
    HostFixture host;
    host.start(); host.supervisor.dispatch(FirstFrame{Generation{1}}); host.advance(5);
    double now = 5.0;
    for (const auto delay : kDefaultRecoveryPolicy.attempt_delays) {
        host.supervisor.dispatch(ProcessExited{Generation{1}});
        now += delay.count();
        host.advance(now);
        REQUIRE(std::holds_alternative<RecreatePlayer>(host.effects.back().payload));
        host.supervisor.dispatch(StreamLoadIssued{Generation{1}, RecoveryTransport::MpegTs});
    }
    host.supervisor.dispatch(ProcessExited{Generation{1}});
    CHECK(host.supervisor.current().name == SupervisorStateName::Failed);
    CHECK(host.supervisor.current().failure == FailureReason::AttemptsExhausted);
    CHECK(host.effects.size() == kDefaultRecoveryPolicy.attempt_delays.size());
}
