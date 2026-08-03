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

}  // namespace coax::core
