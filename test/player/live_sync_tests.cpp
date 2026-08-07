#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <span>

#include "core/policy.hpp"
#include "core/supervisor_host.hpp"
#include "player/live_sync.hpp"
#include "player/live_sync_gate.hpp"
#include "player/live_sync_turn.hpp"

using namespace coax;
using Catch::Approx;

// The control law. Constants come from LiveSyncConfig's ExoPlayer-derived
// defaults, so the numbers below are the shipped policy rather than a fixture.

TEST_CASE("the first update controls immediately and later ones are rate limited") {
    player::LiveSync sync;

    // 0.5s above the 4s target asks for 1.05, which the range caps at 1.03.
    CHECK(sync.update(4.5, 100.0) == Approx(1.03));

    // Inside the one-second interval nothing is written, even though the error
    // has changed enough to compute a different speed.
    CHECK_FALSE(sync.update(4.2, 100.9));
    CHECK(sync.speed() == Approx(1.03));

    CHECK(sync.update(4.2, 101.0) == Approx(1.02));
}

TEST_CASE("speed is clamped to the configured range at both ends") {
    player::LiveSync fast;
    CHECK(fast.update(100.0, 0.0) == Approx(1.03));

    player::LiveSync slow;
    CHECK(slow.update(0.0, 0.0) == Approx(0.97));
}

TEST_CASE("the deadband holds unity within 20ms of the target") {
    player::LiveSync sync;

    // 10ms of error is noise, so the controller does not chase it.
    CHECK_FALSE(sync.update(4.010, 0.0));
    CHECK(sync.speed() == Approx(1.0));

    // Just outside the band it does act, however small the correction.
    CHECK(sync.update(4.030, 1.0) == Approx(1.003));

    // And returning inside the band takes the correction back off.
    CHECK(sync.update(4.010, 2.0) == Approx(1.0));
}

TEST_CASE("a rebuffer concedes 500ms and recomputes without waiting out the interval") {
    player::LiveSync sync;
    CHECK_FALSE(sync.update(4.0, 0.0));
    CHECK(sync.target_offset_seconds() == Approx(4.0));

    sync.notify_rebuffer();
    CHECK(sync.target_offset_seconds() == Approx(4.5));

    // 0.1s later, well inside the rate limit: the target moved, so the speed
    // that was computed against the old one is stale.
    CHECK(sync.update(4.0, 0.1) == Approx(0.97));
}

TEST_CASE("the target is bounded however many rebuffers arrive") {
    player::LiveSync sync;
    for (int i = 0; i < 200; ++i) sync.notify_rebuffer();
    CHECK(sync.target_offset_seconds() == Approx(30.0));
}

TEST_CASE("reset returns the target, the speed and the rate limiter to their initial state") {
    player::LiveSync sync;
    CHECK(sync.update(10.0, 0.0) == Approx(1.03));
    sync.notify_rebuffer();
    sync.notify_rebuffer();
    CHECK(sync.target_offset_seconds() == Approx(5.0));

    sync.reset();
    CHECK(sync.target_offset_seconds() == Approx(4.0));
    CHECK(sync.speed() == Approx(1.0));

    // Learned latency is not carried across channels, and neither is the
    // interval: the next channel's first sample controls immediately.
    CHECK(sync.update(4.5, 0.1) == Approx(1.03));
}

TEST_CASE("holding unity takes an installed correction off exactly once") {
    player::LiveSync sync;
    CHECK(sync.update(0.0, 0.0) == Approx(0.97));

    CHECK(sync.hold_unity_speed() == Approx(1.0));
    CHECK(sync.speed() == Approx(1.0));

    // Nothing further to write while it stays held.
    CHECK_FALSE(sync.hold_unity_speed());
    CHECK_FALSE(sync.hold_unity_speed());
}

TEST_CASE("a correction is reissued after a hold, not suppressed as unchanged") {
    player::LiveSync sync;
    CHECK(sync.update(0.0, 0.0) == Approx(0.97));
    CHECK(sync.hold_unity_speed() == Approx(1.0));

    // The same conditions compute the same 0.97 as before the hold. It has to
    // be written again: playback is at 1.0 now, so treating it as an unchanged
    // speed would leave the controller's record and mpv's disagreeing forever.
    CHECK(sync.update(0.0, 1.0) == Approx(0.97));
}

// The gate. These are the rules that decide what a turn may learn from mpv's
// cache signalling, which is where both live-sync defects lived.

namespace {

player::LiveSyncSample playing(std::optional<double> buffered) {
    return {.buffered_seconds     = buffered,
            .paused_for_cache     = false,
            .core_idle            = false,
            .playback_established = true};
}

player::LiveSyncSample buffering(bool playback_established) {
    return {.buffered_seconds     = std::nullopt,
            .paused_for_cache     = true,
            .core_idle            = false,
            .playback_established = playback_established};
}

// A load that has been issued and produced nothing yet: the core is idle, the
// cache is not yet holding playback back, and the supervisor has confirmed
// nothing.
player::LiveSyncSample loading() {
    return {.buffered_seconds     = std::nullopt,
            .paused_for_cache     = false,
            .core_idle            = true,
            .playback_established = false};
}

// The turn mpv publishes as playback begins: a picture is up and the cache is
// momentarily not holding it back, but the opening fill has not finished and
// the supervisor has not confirmed anything.
player::LiveSyncSample starting(std::optional<double> buffered) {
    return {.buffered_seconds     = buffered,
            .paused_for_cache     = false,
            .core_idle            = false,
            .playback_established = false};
}

}  // namespace

TEST_CASE("the initial fill before playback is established is not a rebuffer") {
    player::LiveSyncGate gate;

    // cache-pause-initial=yes publishes paused-for-cache for a normal opening
    // fill. Nothing has been interrupted, so nothing may be conceded.
    CHECK_FALSE(gate.observe(buffering(false)).rebuffered);
    CHECK_FALSE(gate.observe(buffering(false)).rebuffered);

    CHECK_FALSE(gate.observe(playing(4.0)).rebuffered);

    // The same state, once playback is established, is a real underrun.
    CHECK(gate.observe(buffering(true)).rebuffered);
}

TEST_CASE("an initial fill that reaches the gate with the first frame is not a rebuffer") {
    player::LiveSyncGate gate;

    CHECK_FALSE(gate.observe(loading()).rebuffered);

    // The turn the 2026-08-07 session produced on 15 of 18 channel starts: the
    // application set its first-frame flag from this turn's events and then
    // read a pause property that was already true. A rule that asks only
    // whether a frame has been seen answers yes here, which is how the first
    // attempt was defeated in the shipped wiring while passing in isolation.
    CHECK_FALSE(gate.observe(buffering(false)).rebuffered);
    CHECK_FALSE(gate.observe(buffering(false)).rebuffered);

    CHECK_FALSE(gate.observe(playing(3.9)).rebuffered);
    CHECK(gate.observe(buffering(true)).rebuffered);
}

TEST_CASE("a momentary unpause as playback begins does not establish it") {
    player::LiveSyncGate gate;

    CHECK_FALSE(gate.observe(loading()).rebuffered);
    CHECK_FALSE(gate.observe(buffering(false)).rebuffered);

    // The 2026-08-08 run against a live HLS source. mpv released
    // paused-for-cache for a single frame turn as the picture came up, then
    // re-entered the same opening fill a millisecond later. Reading "a frame,
    // and a turn that is not filling" as established playback arms on the
    // first of these and charges the second, which is a rising edge on a fill
    // that never actually ended.
    CHECK_FALSE(gate.observe(starting(std::nullopt)).rebuffered);
    CHECK_FALSE(gate.observe(buffering(false)).rebuffered);
    CHECK_FALSE(gate.observe(starting(0.8)).rebuffered);
    CHECK_FALSE(gate.observe(buffering(false)).rebuffered);

    // The supervisor confirms steady only after a healthy window, which the
    // opening fill cannot manufacture.
    CHECK_FALSE(gate.observe(playing(4.0)).rebuffered);
    CHECK(gate.observe(buffering(true)).rebuffered);
}

TEST_CASE("a stall is counted on entry rather than once per turn") {
    player::LiveSyncGate gate;
    REQUIRE_FALSE(gate.observe(playing(4.0)).rebuffered);

    CHECK(gate.observe(buffering(true)).rebuffered);
    CHECK_FALSE(gate.observe(buffering(true)).rebuffered);
    CHECK_FALSE(gate.observe(buffering(true)).rebuffered);

    // Leaving and re-entering is a second one.
    CHECK_FALSE(gate.observe(playing(4.0)).rebuffered);
    CHECK(gate.observe(buffering(true)).rebuffered);
}

TEST_CASE("a recovery reopen concedes nothing for its own fill") {
    player::LiveSyncGate gate;
    REQUIRE_FALSE(gate.observe(playing(4.0)).rebuffered);
    REQUIRE(gate.observe(buffering(true)).rebuffered);

    // Recovery reopens the stream: the load is no longer established, but the
    // controller and this gate deliberately keep their learned state, so
    // reset() is not called here.
    CHECK_FALSE(gate.observe(loading()).rebuffered);

    // Generation 17's shape. The reopen's fill and its recovered first frame
    // reached the application together, and it charged Rebuffer #6 in the same
    // millisecond as the recovered first frame. The previous version of this
    // case fed a sequence the runtime does not produce, so it passed while
    // asserting behaviour the session had already falsified.
    CHECK_FALSE(gate.observe(buffering(false)).rebuffered);
    CHECK_FALSE(gate.observe(starting(1.0)).rebuffered);
    CHECK_FALSE(gate.observe(buffering(false)).rebuffered);

    // Once the recovered load is established, underruns count as before.
    CHECK_FALSE(gate.observe(playing(4.0)).rebuffered);
    CHECK(gate.observe(buffering(true)).rebuffered);
}

TEST_CASE("the controller is left alone while the cache drains or the core is idle") {
    player::LiveSyncGate gate;

    // Buffered duration is available here, and still must not be controlled
    // on: it is draining rather than mistimed.
    const auto stalled = gate.observe({.buffered_seconds     = 0.5,
                                       .paused_for_cache     = true,
                                       .core_idle            = false,
                                       .playback_established = true});
    CHECK_FALSE(stalled.control_input);
    CHECK_FALSE(stalled.hold_unity_speed);

    const auto idle = gate.observe({.buffered_seconds     = 4.0,
                                    .paused_for_cache     = false,
                                    .core_idle            = true,
                                    .playback_established = true});
    CHECK_FALSE(idle.control_input);
    CHECK_FALSE(idle.hold_unity_speed);
}

TEST_CASE("unavailable telemetry asks for unity instead of being read as zero") {
    player::LiveSyncGate gate;

    const auto lost = gate.observe(playing(std::nullopt));
    CHECK(lost.hold_unity_speed);
    CHECK_FALSE(lost.control_input);

    // A real zero reading is a different observation and is controlled on.
    const auto empty = gate.observe(playing(0.0));
    CHECK_FALSE(empty.hold_unity_speed);
    REQUIRE(empty.control_input);
    CHECK(*empty.control_input == Approx(0.0));

    const auto recovered = gate.observe(playing(4.2));
    CHECK_FALSE(recovered.hold_unity_speed);
    REQUIRE(recovered.control_input);
    CHECK(*recovered.control_input == Approx(4.2));
}

TEST_CASE("reset drops the previous load's pause state") {
    player::LiveSyncGate gate;
    REQUIRE_FALSE(gate.observe(playing(4.0)).rebuffered);
    REQUIRE(gate.observe(buffering(true)).rebuffered);
    REQUIRE_FALSE(gate.observe(buffering(true)).rebuffered);

    gate.reset();

    // A new channel starts from a not-paused baseline rather than inheriting
    // the state the last one happened to end in.
    CHECK(gate.observe(buffering(true)).rebuffered);
}

// Gate and controller together, over the sequences that produced the two
// defects. App::update_live_sync is these two objects and nothing else.

namespace {

// Mirrors App::update_live_sync: gate first, then whichever of the two
// controller entry points the step selected. Returns any speed to install.
std::optional<double> tick(player::LiveSyncGate& gate, player::LiveSync& sync,
                           const player::LiveSyncSample& sample, double now_seconds) {
    const auto step = gate.observe(sample);
    if (step.rebuffered) sync.notify_rebuffer();
    if (step.hold_unity_speed) return sync.hold_unity_speed();
    if (step.control_input) return sync.update(*step.control_input, now_seconds);
    return std::nullopt;
}

}  // namespace

TEST_CASE("a channel change costs no latency before playback is established") {
    player::LiveSyncGate gate;
    player::LiveSync     sync;

    // Zap: mpv pauses to fill, publishes no buffered duration yet, and the
    // supervisor has confirmed nothing. Previously this conceded 500ms per
    // channel change.
    for (int turn = 0; turn < 30; ++turn) {
        CHECK_FALSE(tick(gate, sync, buffering(false), turn * 0.1));
    }
    CHECK(sync.target_offset_seconds() == Approx(4.0));
    CHECK(sync.speed() == Approx(1.0));
}

TEST_CASE("losing buffered duration holds unity instead of installing the minimum speed") {
    player::LiveSyncGate gate;
    player::LiveSync     sync;

    // Settled a little under target, so a real correction is running.
    REQUIRE(tick(gate, sync, playing(3.8), 0.0) == Approx(0.98));
    REQUIRE(sync.speed() == Approx(0.98));

    // mpv stops reporting the property. Read as 0.0 this is a -4s error, which
    // pins 0.97x and accumulates roughly 108 seconds of latency per hour.
    CHECK(tick(gate, sync, playing(std::nullopt), 1.0) == Approx(1.0));
    for (int turn = 2; turn < 60; ++turn) {
        CHECK_FALSE(tick(gate, sync, playing(std::nullopt), turn));
    }
    CHECK(sync.speed() == Approx(1.0));

    // The target is untouched by the outage: nothing rebuffered.
    CHECK(sync.target_offset_seconds() == Approx(4.0));

    // When the property comes back the controller resumes from the real value.
    CHECK(tick(gate, sync, playing(4.5), 60.0) == Approx(1.03));
}

// The application turn. App::process_player_events() runs before
// App::update_live_sync() in the same iteration of the frame loop, so a first
// frame and a pause property that was already true arrive as one observation
// rather than in sequence. That ordering, plus the momentary unpause mpv
// publishes as a picture comes up, is what defeated two successive guards in the
// shipped wiring while every isolated gate case above passed. These drive the
// object the application drives, in the order it drives it: the event drain, the
// supervisor's state change, then the property snapshot.

namespace {

constexpr core::Generation kLoad{7};

player::PlayerEvent first_frame(core::Generation generation) {
    return {generation, player::FirstPlaybackStart{}};
}

// The drain, for the turns where exactly one event lands.
std::array<player::PlayerEvent, 1> drain(player::PlayerEvent event) { return {event}; }

// A load has been issued and mpv has produced nothing yet.
player::Diagnostics opening() {
    player::Diagnostics diagnostics;
    diagnostics.core_idle = true;
    return diagnostics;
}

// The opening fill holding playback back. cache-pause-initial=yes publishes the
// same paused-for-cache the underrun path uses, and buffered duration is one of
// the readings mpv does not have yet.
player::Diagnostics filling() {
    player::Diagnostics diagnostics;
    diagnostics.paused_for_cache = true;
    return diagnostics;
}

// Neither filling nor idle. This is both the turn a picture comes up on -- mpv
// releases paused-for-cache as playback begins, before the opening fill has
// finished, and re-enters it a frame later -- and ordinary playback afterwards.
player::Diagnostics unpaused(std::optional<double> buffered) {
    player::Diagnostics diagnostics;
    diagnostics.cache_duration_seconds = buffered;
    return diagnostics;
}

// Mirrors App::update_live_sync preceded by App::process_player_events: this
// turn's events, then this turn's properties, then whichever controller entry
// point the step selected.
player::LiveSyncStep frame(player::LiveSyncTurn& turn, player::LiveSync& sync,
                           std::span<const player::PlayerEvent> events,
                           const player::Diagnostics& diagnostics, double now_seconds) {
    turn.observe_events(events, kLoad);
    const auto step = turn.observe(diagnostics);
    if (step.rebuffered) sync.notify_rebuffer();
    if (step.hold_unity_speed) sync.hold_unity_speed();
    else if (step.control_input) sync.update(*step.control_input, now_seconds);
    return step;
}

// The opening sequence the 2026-08-08 run produced against a live HLS source,
// as the application saw it: seven seconds of fill, then one turn carrying both
// the first frame and a momentary unpause, then the fill resuming. Every turn
// here precedes the supervisor's steady confirmation.
void open_channel(player::LiveSyncTurn& turn, player::LiveSync& sync, double start) {
    frame(turn, sync, {}, opening(), start);
    for (int t = 1; t < 20; ++t) frame(turn, sync, {}, filling(), start + t * 0.1);

    // The collapsed turn: this turn's drain reports the first frame, and this
    // turn's properties are read immediately after it.
    frame(turn, sync, drain(first_frame(kLoad)), unpaused(std::nullopt), start + 2.0);
    frame(turn, sync, {}, filling(), start + 2.001);
    frame(turn, sync, {}, unpaused(0.8), start + 2.002);
    frame(turn, sync, {}, filling(), start + 2.003);
}

}  // namespace

TEST_CASE("a channel start holds its opening target through first frame and fill") {
    player::LiveSyncTurn turn;
    player::LiveSync     sync;
    turn.begin_load();

    open_channel(turn, sync, 0.0);
    CHECK(turn.first_frame_seen());
    CHECK_FALSE(turn.playback_established());

    // The user-visible point. 15 of the 18 channel starts in the 2026-08-07
    // session logged Rebuffer #1 within a millisecond of first frame, and the
    // 2026-08-08 run still did with a first-frame guard in place. A channel now
    // opens at the 4.0s initial target instead of stepping to 4.5s before
    // anything the viewer was watching had been interrupted.
    CHECK(sync.target_offset_seconds() == Approx(4.0));
}

TEST_CASE("a stall after the supervisor confirms steady still concedes latency") {
    player::LiveSyncTurn turn;
    player::LiveSync     sync;
    turn.begin_load();
    open_channel(turn, sync, 0.0);

    // Five healthy seconds later, the supervisor confirms the load.
    turn.note_playback_established();
    REQUIRE_FALSE(frame(turn, sync, {}, unpaused(4.0), 7.0).rebuffered);

    // Now the same engine state means the viewer lost picture.
    CHECK(frame(turn, sync, {}, filling(), 8.0).rebuffered);
    CHECK(sync.target_offset_seconds() == Approx(4.5));
}

TEST_CASE("a recovery reopen charges nothing for its own opening sequence") {
    player::LiveSyncTurn turn;
    player::LiveSync     sync;
    turn.begin_load();
    open_channel(turn, sync, 0.0);
    turn.note_playback_established();
    REQUIRE_FALSE(frame(turn, sync, {}, unpaused(4.0), 7.0).rebuffered);
    REQUIRE(frame(turn, sync, {}, filling(), 8.0).rebuffered);
    REQUIRE(sync.target_offset_seconds() == Approx(4.5));

    // Recovery reopens the stream: begin_load() and deliberately not reset(),
    // because the gate's pause state and the controller's learned target are
    // meant to survive the episode. The reopen keeps the 4.5s it earned.
    turn.begin_load();
    CHECK_FALSE(turn.playback_established());

    // Generation 17 charged Rebuffer #6 in the same millisecond as its
    // recovered first frame. Nothing was interrupted: the viewer had already
    // lost picture, and this is the load that gives it back.
    open_channel(turn, sync, 9.0);
    CHECK(sync.target_offset_seconds() == Approx(4.5));

    // The recovered load earns its own underruns once the supervisor confirms
    // it again -- which it must do from scratch, on this load's own evidence.
    turn.note_playback_established();
    CHECK_FALSE(frame(turn, sync, {}, unpaused(3.0), 15.0).rebuffered);
    CHECK(frame(turn, sync, {}, filling(), 16.0).rebuffered);
    CHECK(sync.target_offset_seconds() == Approx(5.0));
}

TEST_CASE("a first frame belonging to a superseded load is not this load's") {
    player::LiveSyncTurn turn;
    player::LiveSync     sync;
    turn.begin_load();

    // A late frame from the load the user replaced. It says nothing about
    // whether the load now filling has shown anything, and the health fold
    // reads this flag to decide whether a stall is an open or a progress one.
    const auto superseded = core::Generation{kLoad.value() - 1};
    CHECK_FALSE(frame(turn, sync, drain(first_frame(superseded)), filling(), 0.0).rebuffered);
    CHECK_FALSE(turn.first_frame_seen());
    CHECK(sync.target_offset_seconds() == Approx(4.0));
}

// The revised design's critical integration. The cases above hand
// LiveSyncTurn its arming decision directly, which is the right shape for the
// gate's own rules but assumes the supervisor's steady confirmation means what
// it claims. It has to be driven for real: `Steady` is reached by a deadline,
// and the fold that would otherwise restart that deadline emits its interrupted
// edge only on a healthy-to-degraded transition. An opening fill that begins
// before first frame is already degraded when the window is armed.

namespace {

class FrameClock final : public core::SupervisorClock {
public:
    core::TimePoint current{};
    [[nodiscard]] core::TimePoint now() const override { return current; }
};

// App::run()'s frame loop, reduced to the parts that decide whether a turn may
// concede latency, in the order run() uses them: the player-event drain, the
// supervisor poll, the interval health sample that dispatches cache state, and
// the live-sync update. The poll deliberately precedes the sample, as it does
// in the application.
struct AppLoop {
    FrameClock                clock;
    player::LiveSyncTurn      turn;
    player::LiveSync          sync;
    core::PlaybackSupervisor  supervisor;
    core::Generation          generation{1};
    std::optional<bool>       last_cache_state_dispatched;
    core::TimePoint           next_health_sample{};
    int                       rebuffers = 0;

    AppLoop()
        : supervisor(clock, {
              .on_effect = {},
              // App::on_supervisor_state_changed.
              .on_state_changed = [this](const core::SupervisorState& state) {
                  if (state.name == core::SupervisorStateName::Steady &&
                      state.generation == generation) {
                      turn.note_playback_established();
                  }
              },
              .on_transition = {}}) {}

    // App::play() followed by App::begin_health_load().
    void play() {
        supervisor.dispatch(core::ChannelRequested{generation});
        supervisor.dispatch(core::StreamLoadIssued{generation,
                                                   core::RecoveryTransport::MpegTs});
        turn.begin_load();
        last_cache_state_dispatched.reset();
        next_health_sample = clock.current + core::kDefaultHealthPolicy.sample_interval;
    }

    void tick(double at, const player::Diagnostics& diagnostics, bool frame_started = false) {
        clock.current = core::TimePoint{core::seconds(at)};

        if (frame_started) {
            turn.observe_events(drain(first_frame(generation)), generation);
            supervisor.dispatch(core::FirstFrame{generation});
        }
        supervisor.poll();

        if (clock.current >= next_health_sample) {
            next_health_sample = clock.current + core::kDefaultHealthPolicy.sample_interval;
            if (!last_cache_state_dispatched ||
                *last_cache_state_dispatched != diagnostics.paused_for_cache) {
                last_cache_state_dispatched = diagnostics.paused_for_cache;
                supervisor.dispatch(core::CacheState{generation,
                                                     diagnostics.paused_for_cache});
            }
        }

        const auto step = turn.observe(diagnostics);
        if (step.rebuffered) {
            ++rebuffers;
            sync.notify_rebuffer();
        }
        if (step.hold_unity_speed) sync.hold_unity_speed();
        else if (step.control_input) sync.update(*step.control_input, at);
    }
};

}  // namespace

TEST_CASE("an opening fill that outlasts the steady window still concedes nothing") {
    AppLoop app;
    app.play();

    // The opening fill starts before anything has been shown, so the health
    // fold classifies the load degraded from the outset.
    for (double t = 0.5; t < 6.9; t += 0.5) app.tick(t, filling());

    // The picture comes up mid-fill and arms the five-second window.
    app.tick(6.9, unpaused(std::nullopt), /*frame_started=*/true);
    REQUIRE(app.turn.first_frame_seen());

    // The fill outlasts that window. There is no healthy-to-degraded transition
    // left to emit, so PlaybackInterrupted never restarts it; only the reducer
    // refusing to confirm while the cache is paused keeps the load unconfirmed.
    for (double t = 7.0; t < 14.0; t += 0.5) app.tick(t, filling());
    CHECK_FALSE(app.turn.playback_established());

    // The fill then oscillates, exactly as it does at playback start. Charging
    // any of these is the defect this whole change exists to remove.
    app.tick(14.0, unpaused(0.9));
    app.tick(14.001, filling());
    app.tick(14.5, unpaused(1.2));
    app.tick(14.501, filling());
    CHECK(app.rebuffers == 0);
    CHECK(app.sync.target_offset_seconds() == Approx(4.0));

    // Five seconds without an observed fill is the first thing that confirms
    // the load.
    for (double t = 15.0; t < 21.0; t += 0.5) app.tick(t, unpaused(4.0));
    REQUIRE(app.turn.playback_established());

    // And only now does a stall mean the viewer lost picture.
    app.tick(21.0, filling());
    CHECK(app.rebuffers == 1);
    CHECK(app.sync.target_offset_seconds() == Approx(4.5));
}
