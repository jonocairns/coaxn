#include "player/delivery_telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace coax::player {
namespace {

double seconds(core::Duration value) {
    return std::chrono::duration<double>(value).count();
}

// Nearest-rank percentile over an already sorted, non-empty sequence.
double percentile(const std::vector<double>& sorted, double fraction) {
    const auto rank = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(sorted.size())));
    return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
}

}  // namespace

std::optional<DeliveryLoadSummary> DeliveryTelemetry::begin_load(
    core::Generation generation, core::LoadAttempt load_attempt, core::TimePoint issued_at) {
    auto previous = end_load();
    load_ = Load{.generation = generation, .load_attempt = load_attempt, .issued_at = issued_at};
    return previous;
}

std::optional<DeliveryLoadSummary> DeliveryTelemetry::end_load() {
    if (!load_) return std::nullopt;
    const auto load = std::move(*load_);
    load_.reset();
    // A load that never produced a readable sample says nothing about delivery.
    if (!load.last_observed_at) return std::nullopt;
    return summarize(load);
}

std::optional<core::Duration> DeliveryTelemetry::observe(const DeliverySample& sample) {
    if (!load_ || sample.generation != load_->generation ||
        sample.load_attempt != load_->load_attempt) return std::nullopt;
    auto& load = *load_;
    if (load.last_observed_at && sample.observed_at < *load.last_observed_at) return std::nullopt;
    load.last_observed_at = sample.observed_at;

    if (sample.buffer_seconds && load.first_data_at) {
        load.buffer_min_seconds = std::min(load.buffer_min_seconds.value_or(*sample.buffer_seconds),
                                           *sample.buffer_seconds);
        load.buffer_max_seconds = std::max(load.buffer_max_seconds.value_or(*sample.buffer_seconds),
                                           *sample.buffer_seconds);
    }
    if (!sample.cache_end_seconds) return std::nullopt;

    const auto previous = load.last_cache_end_seconds;
    load.last_cache_end_seconds = sample.cache_end_seconds;
    // A readable cache end means the demuxer holds packets, unless the buffer
    // reading says it has already drained them.
    const bool first_data = !load.first_data_at &&
        (!sample.buffer_seconds || *sample.buffer_seconds > kArrivalEpsilonSeconds);
    if (first_data) {
        load.first_data_at = sample.observed_at;
        load.last_arrival_at = sample.observed_at;
        if (sample.buffer_seconds) {
            load.buffer_min_seconds = *sample.buffer_seconds;
            load.buffer_max_seconds = *sample.buffer_seconds;
        }
        return sample.observed_at - load.issued_at;
    }
    // Either direction counts: a timestamp reset still means packets arrived.
    if (previous && load.last_arrival_at &&
        std::abs(*sample.cache_end_seconds - *previous) > kArrivalEpsilonSeconds) {
        load.arrival_gaps_seconds.push_back(seconds(sample.observed_at - *load.last_arrival_at));
        load.last_arrival_at = sample.observed_at;
    }
    return std::nullopt;
}

std::optional<core::Duration> DeliveryTelemetry::input_silence(core::TimePoint now) const {
    if (!load_ || !load_->last_arrival_at || now < *load_->last_arrival_at) return std::nullopt;
    return now - *load_->last_arrival_at;
}

DeliveryLoadSummary DeliveryTelemetry::summarize(const Load& load) const {
    DeliveryLoadSummary summary{
        .generation = load.generation,
        .load_attempt = load.load_attempt,
        .observed_for = *load.last_observed_at - load.issued_at,
        .buffer_min_seconds = load.buffer_min_seconds,
        .buffer_max_seconds = load.buffer_max_seconds,
    };
    if (load.first_data_at) summary.load_to_first_data = *load.first_data_at - load.issued_at;
    if (!load.arrival_gaps_seconds.empty()) {
        auto sorted = load.arrival_gaps_seconds;
        std::sort(sorted.begin(), sorted.end());
        summary.arrival_gaps = DeliveryGapStats{
            .count = sorted.size(),
            .p50_seconds = percentile(sorted, 0.50),
            .p90_seconds = percentile(sorted, 0.90),
            .p99_seconds = percentile(sorted, 0.99),
            .max_seconds = sorted.back(),
        };
    }
    return summary;
}

std::string format_delivery_summary(const DeliveryLoadSummary& summary) {
    const auto secs = [](std::optional<double> value) {
        if (!value) return std::string("unavailable");
        std::ostringstream out;
        out << std::fixed << std::setprecision(2) << *value << "s";
        return out.str();
    };
    const auto millis = [](std::optional<core::Duration> value) {
        if (!value) return std::string("unavailable");
        std::ostringstream out;
        out << std::fixed << std::setprecision(0) << seconds(*value) * 1000.0 << "ms";
        return out.str();
    };
    const auto& gaps = summary.arrival_gaps;
    std::ostringstream out;
    out << "Delivery summary generation=" << summary.generation.value()
        << " load-attempt=" << summary.load_attempt.value()
        << " observed-for=" << millis(summary.observed_for)
        << " load-to-first-data=" << millis(summary.load_to_first_data)
        << " arrivals=" << (gaps ? gaps->count : 0)
        << " gap-p50=" << secs(gaps ? std::optional{gaps->p50_seconds} : std::nullopt)
        << " gap-p90=" << secs(gaps ? std::optional{gaps->p90_seconds} : std::nullopt)
        << " gap-p99=" << secs(gaps ? std::optional{gaps->p99_seconds} : std::nullopt)
        << " gap-max=" << secs(gaps ? std::optional{gaps->max_seconds} : std::nullopt)
        << " buffer-min=" << secs(summary.buffer_min_seconds)
        << " buffer-max=" << secs(summary.buffer_max_seconds)
        << " schema=" << summary.schema_version;
    return out.str();
}

}  // namespace coax::player
