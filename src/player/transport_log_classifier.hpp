#pragma once

#include <optional>
#include <string_view>
#include <variant>

#include "core/supervisor.hpp"

namespace coax::player {

struct AuthenticationRejected {};
using TransportLogClassification =
    std::variant<AuthenticationRejected, core::TransportFailureReason>;

// Exact parser for the pinned mpv/FFmpeg runtime only. The caller discards the
// input text immediately; only this sanitized closed classification survives.
std::optional<TransportLogClassification> classify_transport_log(
    std::string_view text, core::RecoveryTransport transport,
    bool file_loaded, bool probed_format_forced);

}  // namespace coax::player
