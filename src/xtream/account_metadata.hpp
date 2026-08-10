#pragma once

#include <string_view>

namespace coax::xtream {

// What the provider advertised for this authenticated account. `advertised`
// distinguishes an absent capability list from a present list containing no
// transport Coax recognises.
struct TransportCapabilities {
    bool advertised = false;
    bool mpeg_ts = false;
    bool hls = false;
};

// Kept separate from capability discovery deliberately: availability is a
// provider fact, while preference is a user/session policy. Phase 2a pins the
// preference to MPEG-TS; later phases may expose the second value only after
// the complete HLS path is safe.
enum class TransportPreference { MpegTs, Hls };

[[nodiscard]] bool supports(const TransportCapabilities& capabilities,
                            TransportPreference preference);
[[nodiscard]] const char* to_string(TransportPreference preference);

enum class AccountMetadataStatus {
    Parsed,
    AuthenticationRejected,
    Unavailable,
};

struct AccountMetadataResult {
    AccountMetadataStatus status = AccountMetadataStatus::Unavailable;
    TransportCapabilities transports;
};

// Parses the credential-bearing player_api response into a closed, normalized
// result. The response body and every provider/account field are discarded.
[[nodiscard]] AccountMetadataResult parse_account_metadata(std::string_view body);

}  // namespace coax::xtream
