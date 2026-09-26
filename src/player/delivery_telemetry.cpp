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

// Nearest-rank percentile over the binned gaps. A bin reports its midpoint,
// never more than the exact maximum.
double percentile(const std::vector<std::uint32_t>& bins, std::size_t count,
                  double max_seconds, double fraction) {
    const auto rank = std::clamp<std::size_t>(
        static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(count))), 1, count);
    std::size_t seen = 0;
    for (std::size_t bin = 0; bin < bins.size(); ++bin) {
        seen += bins[bin];
        if (seen >= rank) {
            if (bin + 1 == bins.size()) return max_seconds;
            return std::min((static_cast<double>(bin) + 0.5) *
                                DeliveryTelemetry::kGapBinSeconds,
                            max_seconds);
        }
    }
    return max_seconds;
}

}  // namespace

std::optional<DeliveryLoadSummary> DeliveryTelemetry::begin_load(
    core::Generation generation, core::LoadAttempt load_attempt, core::TimePoint issued_at) {
    if (load_ && load_->generation == generation && load_->load_attempt == load_attempt) {
        return std::nullopt;
    }
    auto previous = end_load();
    load_ = Load{.generation = generation, .load_attempt = load_attempt, .issued_at = issued_at};
    load_->gap_bins.assign(kGapBins, 0);
    return previous;
}

std::optional<DeliveryLoadSummary> DeliveryTelemetry::end_load() {
    if (!load_) return std::nullopt;
    const auto load = std::move(*load_);
    load_.reset();
    // A load that never produced a sample says nothing about delivery.
    if (!load.last_observed_at) return std::nullopt;
    return summarize(load, DeliveryReportKind::Final, *load.last_observed_at);
}

std::optional<DeliveryLoadSummary> DeliveryTelemetry::snapshot(core::TimePoint now) const {
    if (!load_ || !load_->last_observed_at) return std::nullopt;
    return summarize(*load_, DeliveryReportKind::Snapshot,
                     std::max(now, *load_->last_observed_at));
}

std::optional<core::Duration> DeliveryTelemetry::observe(const DeliverySample& sample) {
    if (!load_ || sample.generation != load_->generation ||
        sample.load_attempt != load_->load_attempt) return std::nullopt;
    auto& load = *load_;
    if (load.last_observed_at && sample.observed_at < *load.last_observed_at) return std::nullopt;
    load.last_observed_at = sample.observed_at;

    if (!sample.cache_end_seconds) {
        if (load.first_data_at) ++load.missing_samples;
        return std::nullopt;
    }
    if (sample.buffer_seconds && load.first_data_at) {
        load.buffer_min_seconds = std::min(load.buffer_min_seconds.value_or(*sample.buffer_seconds),
                                           *sample.buffer_seconds);
        load.buffer_max_seconds = std::max(load.buffer_max_seconds.value_or(*sample.buffer_seconds),
                                           *sample.buffer_seconds);
    }
    const bool at_target = sample.buffer_seconds && sample.buffer_target_seconds &&
        *sample.buffer_seconds >= *sample.buffer_target_seconds - kAtTargetMarginSeconds;

    const auto previous = load.last_cache_end_seconds;
    load.last_cache_end_seconds = sample.cache_end_seconds;
    // A readable cache end means the demuxer holds packets, unless the buffer
    // reading says it has already drained them.
    if (!load.first_data_at) {
        if (sample.buffer_seconds && *sample.buffer_seconds <= kMovementEpsilonSeconds) {
            return std::nullopt;
        }
        load.first_data_at = sample.observed_at;
        load.last_movement_at = sample.observed_at;
        if (!at_target) load.quiet_since = sample.observed_at;
        if (sample.buffer_seconds) {
            load.buffer_min_seconds = *sample.buffer_seconds;
            load.buffer_max_seconds = *sample.buffer_seconds;
        }
        return sample.observed_at - load.issued_at;
    }
    if (!previous) return std::nullopt;

    const double movement = *sample.cache_end_seconds - *previous;
    if (movement < -kMovementEpsilonSeconds) {
        // A timestamp reset. It says the clock changed, not how long the
        // input was quiet, so it neither closes nor opens a silence.
        ++load.timestamp_resets;
        return std::nullopt;
    }
    if (movement <= kMovementEpsilonSeconds) {
        if (!load.quiet_since && !at_target) load.quiet_since = sample.observed_at;
        return std::nullopt;
    }

    if (load.quiet_since) {
        const double gap = seconds(sample.observed_at - *load.quiet_since);
        // The nudge keeps an exact multiple of the bin width, which a double
        // division can land just under, in its own bin.
        const auto bin = std::min(static_cast<std::size_t>(gap / kGapBinSeconds + 1e-9),
                                  kGapBins - 1);
        ++load.gap_bins[bin];
        ++load.gap_count;
        load.gap_max_seconds = std::max(load.gap_max_seconds, gap);
    } else {
        ++load.throttled_gaps;
    }
    load.last_movement_at = sample.observed_at;
    load.quiet_since = at_target ? std::nullopt : std::optional{sample.observed_at};
    return std::nullopt;
}

std::optional<core::Duration> DeliveryTelemetry::input_silence(core::TimePoint now) const {
    if (!load_ || !load_->last_movement_at || now < *load_->last_movement_at) return std::nullopt;
    return now - *load_->last_movement_at;
}

DeliveryLoadSummary DeliveryTelemetry::summarize(
    const Load& load, DeliveryReportKind kind, core::TimePoint reported_at) {
    DeliveryLoadSummary summary{
        .kind = kind,
        .generation = load.generation,
        .load_attempt = load.load_attempt,
        .observed_for = reported_at - load.issued_at,
        .throttled_gaps = load.throttled_gaps,
        .timestamp_resets = load.timestamp_resets,
        .missing_samples = load.missing_samples,
        .buffer_min_seconds = load.buffer_min_seconds,
        .buffer_max_seconds = load.buffer_max_seconds,
    };
    if (load.first_data_at) summary.load_to_first_data = *load.first_data_at - load.issued_at;
    if (load.quiet_since && reported_at >= *load.quiet_since) {
        summary.silent_at_end = reported_at - *load.quiet_since;
    }
    if (load.gap_count > 0) {
        summary.source_gaps = DeliveryGapStats{
            .count = load.gap_count,
            .p50_seconds = percentile(load.gap_bins, load.gap_count, load.gap_max_seconds, 0.50),
            .p90_seconds = percentile(load.gap_bins, load.gap_count, load.gap_max_seconds, 0.90),
            .p99_seconds = percentile(load.gap_bins, load.gap_count, load.gap_max_seconds, 0.99),
            .max_seconds = load.gap_max_seconds,
        };
    }
    return summary;
}

const char* to_string(DeliveryReportKind value) {
    switch (value) {
        case DeliveryReportKind::Final: return "summary";
        case DeliveryReportKind::Snapshot: return "snapshot";
    }
    return "summary";
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
    const auto& gaps = summary.source_gaps;
    const auto gap = [&](double DeliveryGapStats::*field) {
        return secs(gaps ? std::optional{(*gaps).*field} : std::nullopt);
    };
    std::ostringstream out;
    out << "Delivery " << to_string(summary.kind)
        << " generation=" << summary.generation.value()
        << " load-attempt=" << summary.load_attempt.value()
        << " observed-for=" << millis(summary.observed_for)
        << " load-to-first-data=" << millis(summary.load_to_first_data)
        << " source-gaps=" << (gaps ? gaps->count : 0)
        << " gap-p50=" << gap(&DeliveryGapStats::p50_seconds)
        << " gap-p90=" << gap(&DeliveryGapStats::p90_seconds)
        << " gap-p99=" << gap(&DeliveryGapStats::p99_seconds)
        << " gap-max=" << gap(&DeliveryGapStats::max_seconds)
        << " throttled-gaps=" << summary.throttled_gaps
        << " silent-at-end=" << millis(summary.silent_at_end)
        << " timestamp-resets=" << summary.timestamp_resets
        << " missing-samples=" << summary.missing_samples
        << " buffer-min=" << secs(summary.buffer_min_seconds)
        << " buffer-max=" << secs(summary.buffer_max_seconds)
        << " schema=" << summary.schema_version;
    return out.str();
}

}  // namespace coax::player
