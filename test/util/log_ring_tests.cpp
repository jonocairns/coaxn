#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "util/log_ring.hpp"

using coax::log::Ring;

TEST_CASE("an unfilled ring returns what was pushed, oldest first") {
    Ring ring(4);
    REQUIRE(ring.size() == 0);
    REQUIRE(ring.snapshot().empty());

    ring.push("one");
    ring.push("two");
    ring.push("three");

    REQUIRE(ring.size() == 3);
    REQUIRE(ring.snapshot() == std::vector<std::string>{"one", "two", "three"});
}

TEST_CASE("a full ring drops the oldest entry and keeps the order") {
    Ring ring(3);
    for (const char* line : {"a", "b", "c", "d", "e"}) {
        ring.push(line);
    }

    REQUIRE(ring.size() == 3);
    REQUIRE(ring.capacity() == 3);
    REQUIRE(ring.snapshot() == std::vector<std::string>{"c", "d", "e"});
}

TEST_CASE("the wrap point does not disturb the order") {
    // Every offset of head_ within one full lap, since the snapshot reads the
    // storage as two runs and only a wrapped ring has a second one.
    Ring ring(3);
    ring.push("a");
    ring.push("b");
    ring.push("c");
    REQUIRE(ring.snapshot() == std::vector<std::string>{"a", "b", "c"});

    ring.push("d");
    REQUIRE(ring.snapshot() == std::vector<std::string>{"b", "c", "d"});
    ring.push("e");
    REQUIRE(ring.snapshot() == std::vector<std::string>{"c", "d", "e"});
    ring.push("f");
    REQUIRE(ring.snapshot() == std::vector<std::string>{"d", "e", "f"});
    ring.push("g");
    REQUIRE(ring.snapshot() == std::vector<std::string>{"e", "f", "g"});
}

TEST_CASE("a ring of one keeps only the last line") {
    Ring ring(1);
    ring.push("first");
    ring.push("second");
    REQUIRE(ring.snapshot() == std::vector<std::string>{"second"});
}

TEST_CASE("a ring that retains nothing drops every line instead of dividing by zero") {
    Ring ring(0);
    ring.push("dropped");
    ring.push("also dropped");
    REQUIRE(ring.size() == 0);
    REQUIRE(ring.snapshot().empty());
}

TEST_CASE("snapshot_into replaces the buffer's previous contents") {
    Ring ring(4);
    ring.push("a");

    std::vector<std::string> buffer{"stale", "leftover", "entries"};
    ring.snapshot_into(buffer);
    REQUIRE(buffer == std::vector<std::string>{"a"});

    ring.push("b");
    ring.snapshot_into(buffer);
    REQUIRE(buffer == std::vector<std::string>{"a", "b"});
}

TEST_CASE("a snapshot is a copy, so later writes do not touch it") {
    // The defect this class exists to remove: the reader used to hold a
    // reference into storage the writers were reallocating.
    Ring ring(2);
    ring.push("original");

    std::vector<std::string> taken = ring.snapshot();
    for (int i = 0; i < 100; ++i) {
        ring.push("replacement " + std::to_string(i));
    }

    REQUIRE(taken == std::vector<std::string>{"original"});
}

TEST_CASE("a reader snapshotting while writers push sees only whole lines") {
    // Two writers and a reader over a ring small enough that it wraps
    // constantly. Under the old unlocked reference this is the UI thread
    // walking a vector another thread is growing; here the only requirement is
    // that every line observed is one that was actually pushed, intact.
    constexpr int kPerWriter = 5000;
    Ring          ring(64);

    std::atomic<bool> stop{false};
    std::atomic<int>  observed{0};
    std::atomic<bool> corrupted{false};

    std::thread reader([&] {
        std::vector<std::string> buffer;
        while (!stop.load(std::memory_order_relaxed)) {
            ring.snapshot_into(buffer);
            if (buffer.size() > ring.capacity()) {
                corrupted.store(true, std::memory_order_relaxed);
            }
            for (const std::string& line : buffer) {
                if (line.rfind("writer-", 0) != 0) {
                    corrupted.store(true, std::memory_order_relaxed);
                }
                observed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    auto writer = [&](int id) {
        for (int i = 0; i < kPerWriter; ++i) {
            ring.push("writer-" + std::to_string(id) + "-line-" + std::to_string(i));
        }
    };
    std::thread first(writer, 1);
    std::thread second(writer, 2);

    first.join();
    second.join();

    // Nothing above guarantees the reader was scheduled at all: if it first ran
    // after stop was set, it would observe nothing and fail the assertion below
    // with the ring perfectly correct. Both writers have finished, so the ring
    // is full and no longer changing -- the reader's next snapshot is bound to
    // return a full buffer, which makes this wait terminate rather than merely
    // usually terminate.
    while (observed.load(std::memory_order_relaxed) == 0) {
        std::this_thread::yield();
    }

    stop.store(true, std::memory_order_relaxed);
    reader.join();

    REQUIRE_FALSE(corrupted.load());
    REQUIRE(observed.load() > 0);
    REQUIRE(ring.size() == ring.capacity());
}
