#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/playback_types.hpp"

namespace coax::player {

// Evidence for sizing a deliberate lag behind live: how long input normally
// goes quiet between arrivals, how deep the buffer runs, and how long a load
// takes to deliver its first data. Observation only -- nothing here feeds a
// playback or recovery decision.

inline constexpr std::string_view kDeliveryTelemetrySchema = "delivery-observability-v1";

struct DeliverySample {
    core::Generation generation;
    core::LoadAttempt load_attempt;
    core::TimePoint observed_at{};
    std::optional<double> cache_end_seconds;
    std::optional<double> buffer_seconds;
};

struct DeliveryGapStats {
    std::size_t count = 0;
    double p50_seconds = 0.0;
    double p90_seconds = 0.0;
    double p99_seconds = 0.0;
    double max_seconds = 0.0;
};

struct DeliveryLoadSummary {
    core::Generation generation;
    core::LoadAttempt load_attempt;
    core::Duration observed_for{};
    std::optional<core::Duration> load_to_first_data{};
    // Intervals between samples in which the cache end moved. On a provider
    // that delivers continuously these sit at the sample interval; on one that
    // delivers in chunks they measure the silence between chunks.
    std::optional<DeliveryGapStats> arrival_gaps{};
    std::optional<double> buffer_min_seconds{};
    std::optional<double> buffer_max_seconds{};
    std::string_view schema_version = kDeliveryTelemetrySchema;
};

class DeliveryTelemetry {
public:
    // Movement below this is sampling noise rather than an arrival.
    static constexpr double kArrivalEpsilonSeconds = 0.05;

    // Starts observing a load. Returns the summary of the load it replaces,
    // if that load was observed at all.
    std::optional<DeliveryLoadSummary> begin_load(
        core::Generation generation, core::LoadAttempt load_attempt, core::TimePoint issued_at);
    std::optional<DeliveryLoadSummary> end_load();

    // Returns the time from load issue to first data when this sample is the
    // first one to show data, and nullopt otherwise.
    std::optional<core::Duration> observe(const DeliverySample& sample);

    // How long the cache end has been still, measured from the last arrival.
    // Absent until the load has delivered anything.
    [[nodiscard]] std::optional<core::Duration> input_silence(core::TimePoint now) const;

private:
    struct Load {
        core::Generation generation;
        core::LoadAttempt load_attempt;
        core::TimePoint issued_at{};
        std::optional<core::TimePoint> first_data_at{};
        std::optional<core::TimePoint> last_arrival_at{};
        std::optional<core::TimePoint> last_observed_at{};
        std::optional<double> last_cache_end_seconds{};
        std::optional<double> buffer_min_seconds{};
        std::optional<double> buffer_max_seconds{};
        std::vector<double> arrival_gaps_seconds{};
    };

    [[nodiscard]] DeliveryLoadSummary summarize(const Load& load) const;

    std::optional<Load> load_;
};

std::string format_delivery_summary(const DeliveryLoadSummary& summary);

}  // namespace coax::player
