#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "core/playback_types.hpp"

namespace coax::core {

// The presentation surface's own policy: which swap-chain notifications mean a
// new object, and how often a lost graphics device may be rebuilt before the
// application admits it is not coming back.
//
// Nothing here names a Windows type. The addresses are carried as integers
// because that is how mpv publishes them, and because the decision they feed is
// about identity, not about dereferencing anything.

// mpv publishes its composition swap chain as a bare address with no documented
// COM ownership or lifetime contract. An address alone cannot answer "is this
// the object already attached?", because a destroyed swap chain and its
// replacement can occupy the same allocation. The epoch supplies the missing
// half: it is bumped at every point mpv can tear down or reconfigure its video
// output, which is the only boundary across which an address can be reused.
struct SwapchainIdentity {
    std::uint64_t address = 0;
    std::uint64_t epoch = 0;

    [[nodiscard]] constexpr bool present() const { return address != 0; }
    constexpr bool operator==(const SwapchainIdentity&) const = default;
};

// Positive swap-chain publications require the presentation owner to expect
// video content. A null publication is always allowed to retire content.
[[nodiscard]] constexpr bool swapchain_publication_allowed(
    bool video_content_expected, const SwapchainIdentity& incoming) {
    return video_content_expected || !incoming.present();
}

enum class SwapchainTransition {
    // A duplicate notification for the object already attached. mpv re-reports
    // an unchanged property on reconfiguration, and re-attaching on every event
    // would churn the visual tree for nothing.
    Ignore,
    // Nothing was attached: take a reference and set the visual's content.
    Attach,
    // mpv reports no swap chain: clear the content, then release.
    Detach,
    // A different object, whether or not it reuses the old address. The old
    // content must be cleared before the new is set.
    Reattach,
};

[[nodiscard]] constexpr SwapchainTransition decide_swapchain_transition(
    const SwapchainIdentity& attached, const SwapchainIdentity& incoming) {
    if (!incoming.present()) {
        return attached.present() ? SwapchainTransition::Detach : SwapchainTransition::Ignore;
    }
    if (!attached.present()) return SwapchainTransition::Attach;
    // Equal addresses within one epoch are the same object; equal addresses
    // across epochs are two objects that happened to land in the same place.
    return attached == incoming ? SwapchainTransition::Ignore
                                : SwapchainTransition::Reattach;
}

// Which DXGI result reported the loss. Both recover identically; the
// distinction is kept because the two say different things about the machine,
// and a diagnostic that collapses them cannot tell a driver upgrade from a
// hang.
enum class DeviceLossKind { Removed, Reset };

const char* to_string(DeviceLossKind value);

// DXGI reports a lost device on every present and resize from the loss onward,
// so the raw signal repeats for as long as the device stays dead. Recovery is
// driven from the edge: this raises once per episode and stays raised until a
// rebuild clears it.
class DeviceLossLatch {
public:
    // True exactly once per loss episode.
    bool raise(DeviceLossKind kind);
    void clear() { kind_.reset(); }

    [[nodiscard]] bool lost() const { return kind_.has_value(); }
    [[nodiscard]] std::optional<DeviceLossKind> kind() const { return kind_; }

private:
    std::optional<DeviceLossKind> kind_;
};

struct PresentationRebuildPolicy {
    // An adapter that is mid-reset refuses device creation for a second or two,
    // so attempts are spaced rather than spent inside that window. Five over
    // roughly five seconds outlasts a driver restart; a machine that still
    // cannot create a device by then is not slow, it is broken.
    std::size_t max_attempts = 5;
    Duration retry_delay = milliseconds(1'000);
};

inline constexpr PresentationRebuildPolicy kDefaultPresentationRebuildPolicy{};

enum class RebuildDecision {
    // Nothing outstanding, or the retry delay has not elapsed.
    Hold,
    // Spend an attempt and rebuild now.
    Attempt,
    // The ceiling is reached. Reported once, so failure is surfaced rather than
    // repeated.
    Exhausted,
};

// Paces and bounds presentation rebuilds. This is not a second recovery state
// machine: it holds no playback state and decides nothing about a channel. It
// exists so that a dead adapter cannot be retried at frame rate or forever,
// while resuming the channel stays with the supervisor.
class PresentationRebuildBudget {
public:
    explicit PresentationRebuildBudget(
        PresentationRebuildPolicy policy = kDefaultPresentationRebuildPolicy)
        : policy_(policy) {}

    // A device loss was observed. Idempotent while a rebuild is outstanding, so
    // a present failure, a resume check and a display change reporting the same
    // dead device collapse into one episode.
    void request(TimePoint now);

    [[nodiscard]] RebuildDecision poll(TimePoint now);

    // The surface is back. The next episode gets a full budget of its own.
    void succeeded();
    void failed(TimePoint now);

    // When poll() will next decide something other than Hold, or absent when it
    // never will without a fresh request(). This is what a frame loop with no
    // present to pace it has to wait on: the retry delay is the only thing
    // between a failed rebuild and the next attempt, so sleeping past it makes
    // recovery late, and not sleeping at all makes it a busy-wait.
    [[nodiscard]] std::optional<TimePoint> next_decision_at(TimePoint now) const;

    [[nodiscard]] bool outstanding() const { return outstanding_; }
    [[nodiscard]] bool exhausted() const { return exhausted_; }
    [[nodiscard]] std::size_t attempts() const { return attempts_; }
    [[nodiscard]] std::size_t attempt_ceiling() const { return policy_.max_attempts; }

private:
    PresentationRebuildPolicy policy_;
    TimePoint due_at_{};
    std::size_t attempts_ = 0;
    bool outstanding_ = false;
    bool exhausted_ = false;
};

// What the presentation surface is doing, as far as the frame loop is
// concerned. The distinction the budget alone cannot make is between the two
// unusable phases: a rebuild that has not happened yet will happen, and a
// budget that has been given up on will not. Collapsing them into one "not
// ready" flag is what leaves a loop waiting forever for an attempt that is
// never coming.
enum class PresentationPhase {
    // A frame can be drawn, and the present that ends it paces the loop.
    Ready,
    // Between a teardown and the attempt that will restore it. Nothing may
    // draw, so nothing presents, so the loop has no throttle of its own.
    Rebuilding,
    // Terminal. The rebuild budget is spent and no further attempt will be
    // made, so there is no presentation deadline left to wake up for.
    Failed,
};

const char* to_string(PresentationPhase value);

// Derived rather than assigned, from the two facts the host already owns: a
// usable surface is always Ready, whatever the budget has been through, so a
// stale flag cannot stop the application drawing.
[[nodiscard]] constexpr PresentationPhase decide_presentation_phase(bool surface_ready,
                                                                   bool rebuild_exhausted) {
    if (surface_ready) return PresentationPhase::Ready;
    return rebuild_exhausted ? PresentationPhase::Failed : PresentationPhase::Rebuilding;
}

// A ceiling on how long the loop may sleep, so the deadlines this decision does
// not model — health sampling, the pending stream-end window, a worker thread
// finishing — still get serviced on a predictable cadence rather than waiting
// on a message that may never arrive. Set to the shortest of them; nothing in
// the frame loop schedules anything sooner than the 50 ms stream-end window.
inline constexpr Duration kFrameWaitCeiling = milliseconds(50);

// How long the frame loop may block before it has to run again. Absent means it
// must not block at all: a present is pending and its vsync wait is the
// throttle. A zero duration means something is already due.
//
// The presentation and supervisor deadlines are passed in rather than assumed,
// because being late for either is the cost of fixing the spin: an adapter that
// comes back is not usable until the rebuild attempt that is waiting on the
// retry delay actually runs.
[[nodiscard]] std::optional<Duration> decide_frame_wait(
    PresentationPhase phase, TimePoint now,
    std::optional<TimePoint> presentation_deadline,
    std::optional<TimePoint> supervisor_deadline,
    Duration ceiling = kFrameWaitCeiling);

}  // namespace coax::core
