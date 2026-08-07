#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

// The bounded in-memory half of the session log, separated from the sink that
// writes it. Kept free of platform headers for the same reason as redact.hpp:
// the sink is Windows-only and untestable by CI, while the sharing rules below
// are exactly the part worth testing.
namespace coax::log {

// A fixed-capacity ring of the most recent lines, written by whichever worker
// threads are logging and read by the UI thread that draws the diagnostics
// panel. Every accessor takes the lock, and readers get a copy: handing out a
// reference to the storage would let the panel iterate a container a worker is
// still growing, discarding and reallocating.
//
// Once full, the oldest entry is overwritten in place rather than erased from
// the front, so a steady stream of log lines neither shifts the whole buffer
// per line nor reallocates its strings.
class Ring {
public:
    explicit Ring(std::size_t capacity);

    // Appends one line, dropping the oldest if that would exceed capacity.
    void push(std::string line);

    // The retained lines, oldest first.
    std::vector<std::string> snapshot() const;

    // The same contents into a buffer the caller owns and keeps. The
    // diagnostics panel redraws every frame while it is open, so reusing one
    // buffer keeps that from allocating a fresh vector each time.
    void snapshot_into(std::vector<std::string>& out) const;

    std::size_t size() const;
    std::size_t capacity() const { return capacity_; }

private:
    mutable std::mutex       mutex_;
    std::vector<std::string> entries_;
    std::size_t              capacity_;
    // Index of the oldest entry, and so of the next slot to overwrite. Stays 0
    // until the ring first fills, while entries_ is still in order.
    std::size_t              head_ = 0;
};

}  // namespace coax::log
