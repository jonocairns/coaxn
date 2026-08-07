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

TEST_CASE("the budget says when it will next decide, so a loop can sleep until then") {
    PresentationRebuildBudget budget;
    // Nothing outstanding is nothing to wake up for.
    CHECK_FALSE(budget.next_decision_at(at(0)).has_value());

    budget.request(at(1));
    CHECK(budget.next_decision_at(at(1)) == at(1));
    REQUIRE(budget.poll(at(1)) == RebuildDecision::Attempt);

    // Mid-attempt and after a failure the answer is the retry delay, which is
    // the whole reason the loop has anything to wait for.
    CHECK(budget.next_decision_at(at(1)) == at(2));
    budget.failed(at(1.2));
    CHECK(budget.next_decision_at(at(1.2)) == at(2.2));

    budget.succeeded();
    CHECK_FALSE(budget.next_decision_at(at(2)).has_value());
}

TEST_CASE("a budget at its ceiling is due immediately, not after another retry delay") {
    // The failure this guards: poll() reports Exhausted regardless of the
    // clock, so reporting due_at_ would put the loop to sleep for a second it
    // is no longer entitled to spend before it can admit it has given up.
    PresentationRebuildBudget budget;
    budget.request(at(0));

    double now = 0.0;
    for (std::size_t attempt = 1; attempt <= kDefaultPresentationRebuildPolicy.max_attempts;
         ++attempt) {
        REQUIRE(budget.poll(at(now)) == RebuildDecision::Attempt);
        budget.failed(at(now));
        now += kDefaultPresentationRebuildPolicy.retry_delay.count();
    }

    CHECK(budget.next_decision_at(at(now)) == at(now));
    REQUIRE(budget.poll(at(now)) == RebuildDecision::Exhausted);

    // And once it has, there is nothing left to wake for at all — including
    // after a later loss, which cannot restart a spent budget.
    CHECK_FALSE(budget.next_decision_at(at(now)).has_value());
    budget.request(at(now + 30));
    CHECK_FALSE(budget.next_decision_at(at(now + 30)).has_value());
}

TEST_CASE("an unusable surface is Rebuilding until the budget gives up, then Failed") {
    CHECK(decide_presentation_phase(false, false) == PresentationPhase::Rebuilding);
    CHECK(decide_presentation_phase(false, true) == PresentationPhase::Failed);

    // A usable surface is Ready whatever the budget has been through. The
    // asymmetry is deliberate: a stale exhaustion flag must not be able to stop
    // an application that can draw from drawing.
    CHECK(decide_presentation_phase(true, false) == PresentationPhase::Ready);
    CHECK(decide_presentation_phase(true, true) == PresentationPhase::Ready);

    CHECK(std::string_view(to_string(PresentationPhase::Ready)) == "ready");
    CHECK(std::string_view(to_string(PresentationPhase::Rebuilding)) == "rebuilding");
    CHECK(std::string_view(to_string(PresentationPhase::Failed)) == "failed");
}

TEST_CASE("a frame that will present must not also sleep") {
    // Present's vsync wait is the loop's throttle. Adding a sleep on top of it
    // would drop frames on a machine with nothing wrong with it.
    CHECK_FALSE(decide_frame_wait(PresentationPhase::Ready, at(0), at(0.1), at(0.2)).has_value());
    CHECK_FALSE(decide_frame_wait(PresentationPhase::Ready, at(0), std::nullopt, std::nullopt)
                    .has_value());
}

TEST_CASE("a frame that will not present sleeps until the nearest deadline") {
    // This is the busy-wait. Nothing reaches Present while the surface is down,
    // so without a wait the loop spins a core across every retry delay — on a
    // machine that has just lost its display adapter.
    const auto rebuilding = decide_frame_wait(PresentationPhase::Rebuilding, at(0), at(0.03),
                                              at(0.2), seconds(1));
    REQUIRE(rebuilding.has_value());
    CHECK(*rebuilding == seconds(0.03));

    // Either deadline can be the nearest one. A wait that ignored the
    // supervisor would make playback recovery late rather than fast.
    const auto supervised = decide_frame_wait(PresentationPhase::Rebuilding, at(0), at(0.2),
                                              at(0.04), seconds(1));
    REQUIRE(supervised.has_value());
    CHECK(*supervised == seconds(0.04));
}

TEST_CASE("the terminal phase still waits, and still wakes for the supervisor") {
    // The indefinite half of the defect: after exhaustion there is no
    // presentation deadline at all, and a loop with nothing to wait for is the
    // one that spins for the lifetime of the process.
    const auto idle = decide_frame_wait(PresentationPhase::Failed, at(0), std::nullopt,
                                        std::nullopt, seconds(1));
    REQUIRE(idle.has_value());
    CHECK(*idle == seconds(1));

    // Playback recovery outlives the presentation surface, so its deadline is
    // still a reason to wake up.
    const auto recovering = decide_frame_wait(PresentationPhase::Failed, at(0), std::nullopt,
                                              at(0.25), seconds(1));
    REQUIRE(recovering.has_value());
    CHECK(*recovering == seconds(0.25));
}

TEST_CASE("the wait is ceilinged, and a deadline already past is no wait at all") {
    // Nothing in the frame loop schedules sooner than the 50 ms stream-end
    // window, so the ceiling bounds how late anything this decision does not
    // model can be.
    const auto ceilinged = decide_frame_wait(PresentationPhase::Rebuilding, at(0), at(30),
                                             at(60));
    REQUIRE(ceilinged.has_value());
    CHECK(*ceilinged == kFrameWaitCeiling);

    // A deadline in the past is no sleep, not a negative one.
    const auto overdue = decide_frame_wait(PresentationPhase::Rebuilding, at(10), at(9),
                                           std::nullopt);
    REQUIRE(overdue.has_value());
    CHECK(*overdue == Duration::zero());

    const auto exact = decide_frame_wait(PresentationPhase::Rebuilding, at(10), at(10),
                                         std::nullopt);
    REQUIRE(exact.has_value());
    CHECK(*exact == Duration::zero());
}

TEST_CASE("device loss kinds are named for the log, not collapsed") {
    CHECK(std::string_view(to_string(DeviceLossKind::Removed)) == "device-removed");
    CHECK(std::string_view(to_string(DeviceLossKind::Reset)) == "device-reset");
}
