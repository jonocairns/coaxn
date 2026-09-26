#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/playback_types.hpp"

namespace coax::player {

// Evidence for sizing playable headroom: how long the cache end goes still
// between movements, how deep the buffer runs, and how long a load takes to
// show its first data. Observation only -- nothing here feeds a playback or
// recovery decision.
//
// Cache-end movement is what the demuxer shows, not packet arrival on the
// wire: transport buffering, demuxing and mpv's own read throttling all shape
// it. Silences that begin with the buffer at its target are reported apart,
// because there mpv has stopped reading rather than the source going quiet.

inline constexpr std::string_view kDeliveryTelemetrySchema = "delivery-observability-v2";

enum class DeliveryReportKind { Final, Snapshot };

struct DeliverySample {
    core::Generation generation;
    core::LoadAttempt load_attempt;
    core::TimePoint observed_at{};
    std::optional<double> cache_end_seconds{};
    std::optional<double> buffer_seconds{};
    std::optional<double> buffer_target_seconds{};
};

struct DeliveryGapStats {
    std::size_t count = 0;
    double p50_seconds = 0.0;
    double p90_seconds = 0.0;
    double p99_seconds = 0.0;
    double max_seconds = 0.0;
};

struct DeliveryLoadSummary {
    DeliveryReportKind kind = DeliveryReportKind::Final;
    core::Generation generation;
    core::LoadAttempt load_attempt;
    core::Duration observed_for{};
    std::optional<core::Duration> load_to_first_data{};
    // Completed silences between cache-end movements while the buffer was
    // below its target. Percentiles come from 50ms bins; the maximum is exact.
    std::optional<DeliveryGapStats> source_gaps{};
    // Completed silences that began with the buffer at its target.
    std::size_t throttled_gaps = 0;
    // The silence still running when the report was made. A load that ends
    // or fails while quiet leaves only this: it is at least this long, not a
    // measured gap, and it is never folded into the percentiles above.
    std::optional<core::Duration> silent_at_end{};
    std::size_t timestamp_resets = 0;
    std::size_t missing_samples = 0;
    std::optional<double> buffer_min_seconds{};
    std::optional<double> buffer_max_seconds{};
    std::string_view schema_version = kDeliveryTelemetrySchema;
};

class DeliveryTelemetry {
public:
    // Movement below this is sampling noise rather than new media.
    static constexpr double kMovementEpsilonSeconds = 0.05;
    // A buffer this close to its target counts as full, so mpv may have
    // stopped reading.
    static constexpr double kAtTargetMarginSeconds = 1.0;
    static constexpr double kGapBinSeconds = 0.05;
    static constexpr std::size_t kGapBins = 2400;  // 120s; longer gaps share the last bin

    // Starts observing a load and returns the final summary of the load it
    // replaces. Beginning the load already being observed continues it: a
    // load revived after the supervisor gave up on it is one physical load.
    std::optional<DeliveryLoadSummary> begin_load(
        core::Generation generation, core::LoadAttempt load_attempt, core::TimePoint issued_at);
    std::optional<DeliveryLoadSummary> end_load();
    // The current load's figures so far, without ending it.
    [[nodiscard]] std::optional<DeliveryLoadSummary> snapshot(core::TimePoint now) const;

    // Returns the time from load issue to first data when this sample is the
    // first one to show data, and nullopt otherwise.
    std::optional<core::Duration> observe(const DeliverySample& sample);

    // How long the cache end has been still, measured from its last forward
    // movement. Absent until the load has shown any data.
    [[nodiscard]] std::optional<core::Duration> input_silence(core::TimePoint now) const;

private:
    struct Load {
        core::Generation generation;
        core::LoadAttempt load_attempt;
        core::TimePoint issued_at{};
        std::optional<core::TimePoint> first_data_at{};
        std::optional<core::TimePoint> last_movement_at{};
        bool last_movement_at_target = false;
        std::optional<core::TimePoint> last_observed_at{};
        std::optional<double> last_cache_end_seconds{};
        std::optional<double> buffer_min_seconds{};
        std::optional<double> buffer_max_seconds{};
        std::vector<std::uint32_t> gap_bins{};
        std::size_t gap_count = 0;
        double gap_max_seconds = 0.0;
        std::size_t throttled_gaps = 0;
        std::size_t timestamp_resets = 0;
        std::size_t missing_samples = 0;
    };

    [[nodiscard]] static DeliveryLoadSummary summarize(
        const Load& load, DeliveryReportKind kind, core::TimePoint reported_at);

    std::optional<Load> load_;
};

std::string format_delivery_summary(const DeliveryLoadSummary& summary);
const char* to_string(DeliveryReportKind value);

}  // namespace coax::player
