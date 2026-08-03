#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "core/presentation.hpp"

using namespace coax::core;

namespace {

TimePoint at(double seconds_value) { return TimePoint{seconds(seconds_value)}; }

constexpr SwapchainIdentity none{};
constexpr SwapchainIdentity chain(std::uint64_t address, std::uint64_t epoch) {
    return SwapchainIdentity{address, epoch};
}

}  // namespace

TEST_CASE("attachment identity is address and epoch together, never address alone") {
    // The failure this exists for: mpv destroys a swap chain and its
    // replacement is allocated at the same address. Compared by address the
    // two are indistinguishable, and the visual keeps a dead object.
    CHECK(decide_swapchain_transition(chain(0x1000, 1), chain(0x1000, 2)) ==
          SwapchainTransition::Reattach);
    CHECK(decide_swapchain_transition(chain(0x1000, 1), chain(0x2000, 1)) ==
          SwapchainTransition::Reattach);

    // Within one epoch the address is sufficient, and mpv re-reporting an
    // unchanged property must not churn the visual tree.
    CHECK(decide_swapchain_transition(chain(0x1000, 1), chain(0x1000, 1)) ==
          SwapchainTransition::Ignore);

    CHECK(decide_swapchain_transition(none, chain(0x1000, 1)) == SwapchainTransition::Attach);
    CHECK(decide_swapchain_transition(chain(0x1000, 1), none) == SwapchainTransition::Detach);
    CHECK(decide_swapchain_transition(none, none) == SwapchainTransition::Ignore);
}

TEST_CASE("a detached identity carries no epoch of its own") {
    // Epoch only means something alongside a live address; a null attachment
    // at any epoch is still nothing attached.
    CHECK(decide_swapchain_transition(chain(0, 7), chain(0x1000, 9)) ==
          SwapchainTransition::Attach);
    CHECK(decide_swapchain_transition(chain(0, 7), chain(0, 9)) == SwapchainTransition::Ignore);
}

TEST_CASE("device loss is raised once per episode, not once per frame") {
    DeviceLossLatch latch;
    CHECK_FALSE(latch.lost());

    CHECK(latch.raise(DeviceLossKind::Removed));
    // Present fails identically for every frame after the loss.
    CHECK_FALSE(latch.raise(DeviceLossKind::Removed));
    CHECK_FALSE(latch.raise(DeviceLossKind::Reset));
    CHECK(latch.lost());
    CHECK(latch.kind() == DeviceLossKind::Removed);

    latch.clear();
    CHECK_FALSE(latch.lost());
    CHECK(latch.raise(DeviceLossKind::Reset));
    CHECK(latch.kind() == DeviceLossKind::Reset);
}

TEST_CASE("the first rebuild attempt is due immediately and repeated losses collapse") {
    PresentationRebuildBudget budget;
    CHECK(budget.poll(at(0)) == RebuildDecision::Hold);

    budget.request(at(1));
    // Present, the resume check and a display change can all report the same
    // dead device; they are one episode, not three.
    budget.request(at(1.01));
    budget.request(at(1.02));
    CHECK(budget.poll(at(1)) == RebuildDecision::Attempt);
    CHECK(budget.attempts() == 1);
    // One attempt outstanding at a time: nothing else is due until it settles.
    CHECK(budget.poll(at(1)) == RebuildDecision::Hold);
}

TEST_CASE("an attempt that is never settled still cannot spin") {
    // poll() spends the retry delay itself rather than trusting the caller to
    // report back. A rebuild that neither succeeds nor fails — an exception on
    // the way out, a path that forgets — must not hand out the next attempt on
    // the next frame, because the frame loop polls at frame rate.
    PresentationRebuildBudget budget;
    budget.request(at(0));
    REQUIRE(budget.poll(at(0)) == RebuildDecision::Attempt);

    CHECK(budget.poll(at(0)) == RebuildDecision::Hold);
    CHECK(budget.poll(at(0.5)) == RebuildDecision::Hold);
    CHECK(budget.attempts() == 1);

    // And the pacing is the ordinary retry delay, not a stall.
    CHECK(budget.poll(at(1.0)) == RebuildDecision::Attempt);
    CHECK(budget.attempts() == 2);
}

TEST_CASE("a failed rebuild waits out the retry delay rather than spinning") {
    PresentationRebuildBudget budget;
    budget.request(at(0));
    REQUIRE(budget.poll(at(0)) == RebuildDecision::Attempt);

    budget.failed(at(0));
    // An adapter mid-reset refuses device creation; retrying at frame rate
    // would spend the whole budget inside that window.
    CHECK(budget.poll(at(0.5)) == RebuildDecision::Hold);
    CHECK(budget.poll(at(0.999)) == RebuildDecision::Hold);
    CHECK(budget.poll(at(1.0)) == RebuildDecision::Attempt);
    CHECK(budget.attempts() == 2);
}

TEST_CASE("rebuilds are bounded and exhaustion is surfaced exactly once") {
    PresentationRebuildBudget budget;
    budget.request(at(0));

    double now = 0.0;
    for (std::size_t attempt = 1; attempt <= kDefaultPresentationRebuildPolicy.max_attempts;
         ++attempt) {
        REQUIRE(budget.poll(at(now)) == RebuildDecision::Attempt);
        CHECK(budget.attempts() == attempt);
        budget.failed(at(now));
        now += kDefaultPresentationRebuildPolicy.retry_delay.count();
    }

    CHECK(budget.poll(at(now)) == RebuildDecision::Exhausted);
    CHECK(budget.exhausted());
    // Reported once. A failed state is surfaced; it is not repeated every frame.
    CHECK(budget.poll(at(now)) == RebuildDecision::Hold);
    CHECK(budget.poll(at(now + 60)) == RebuildDecision::Hold);

    // And it does not quietly restart under a later loss.
    budget.request(at(now + 61));
    CHECK(budget.poll(at(now + 61)) == RebuildDecision::Hold);
}

TEST_CASE("a rebuilt surface returns a full budget to the next episode") {
    PresentationRebuildBudget budget;
    budget.request(at(0));
    REQUIRE(budget.poll(at(0)) == RebuildDecision::Attempt);
    budget.failed(at(0));
    REQUIRE(budget.poll(at(1)) == RebuildDecision::Attempt);
    budget.succeeded();

    CHECK(budget.attempts() == 0);
    CHECK_FALSE(budget.outstanding());
    CHECK(budget.poll(at(2)) == RebuildDecision::Hold);

    // A later loss is a new episode: two attempts spent on the last one do not
    // shorten it.
    budget.request(at(30));
    CHECK(budget.poll(at(30)) == RebuildDecision::Attempt);
    CHECK(budget.attempts() == 1);
}

TEST_CASE("device loss kinds are named for the log, not collapsed") {
    CHECK(std::string_view(to_string(DeviceLossKind::Removed)) == "device-removed");
    CHECK(std::string_view(to_string(DeviceLossKind::Reset)) == "device-reset");
}
