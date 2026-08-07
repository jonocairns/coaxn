#include "util/log_ring.hpp"

#include <utility>

namespace coax::log {

Ring::Ring(std::size_t capacity) : capacity_(capacity) {
    entries_.reserve(capacity_);
}

void Ring::push(std::string line) {
    // A ring that retains nothing is a degenerate configuration rather than an
    // error: dropping the line keeps the modulo below from dividing by zero.
    if (capacity_ == 0) {
        return;
    }

    std::scoped_lock lock(mutex_);
    if (entries_.size() < capacity_) {
        entries_.push_back(std::move(line));
        return;
    }
    entries_[head_] = std::move(line);
    head_           = (head_ + 1) % capacity_;
}

std::vector<std::string> Ring::snapshot() const {
    std::vector<std::string> out;
    snapshot_into(out);
    return out;
}

void Ring::snapshot_into(std::vector<std::string>& out) const {
    std::scoped_lock lock(mutex_);
    out.clear();
    out.reserve(entries_.size());
    // Two runs while the ring is full -- head_ to the end, then the wrapped
    // front. Before it fills, head_ is 0 and the second run is the whole thing.
    out.insert(out.end(), entries_.begin() + static_cast<std::ptrdiff_t>(head_), entries_.end());
    out.insert(out.end(), entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(head_));
}

std::size_t Ring::size() const {
    std::scoped_lock lock(mutex_);
    return entries_.size();
}

}  // namespace coax::log
