#include "xtream/account_metadata.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace coax::xtream {
namespace {

using json = nlohmann::json;

std::string normalize_token(std::string_view value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) return {};

    std::string normalized(first, last);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized;
}

void add_transport(std::string_view value, TransportCapabilities& capabilities) {
    const std::string token = normalize_token(value);
    if (token == "ts" || token == ".ts" || token == "mpegts" || token == "mpeg-ts") {
        capabilities.mpeg_ts = true;
    } else if (token == "hls" || token == "m3u8" || token == ".m3u8") {
        capabilities.hls = true;
    }
}

void parse_transport_values(const json& value, TransportCapabilities& capabilities) {
    if (value.is_array()) {
        capabilities.advertised = true;
        for (const auto& entry : value) {
            if (entry.is_string()) add_transport(entry.get_ref<const std::string&>(), capabilities);
        }
        return;
    }
    if (!value.is_string()) return;
    capabilities.advertised = true;

    const std::string& list = value.get_ref<const std::string&>();
    std::size_t start = 0;
    while (start <= list.size()) {
        const std::size_t comma = list.find(',', start);
        add_transport(std::string_view(list).substr(
                          start, comma == std::string::npos ? std::string::npos : comma - start),
                      capabilities);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

std::optional<bool> authentication_state(const json& value) {
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number_integer()) return value.get<long long>() != 0;
    if (!value.is_string()) return std::nullopt;

    const std::string token = normalize_token(value.get_ref<const std::string&>());
    if (token == "0" || token == "false") return false;
    if (token == "1" || token == "true") return true;
    return std::nullopt;
}

}  // namespace

bool supports(const TransportCapabilities& capabilities, TransportPreference preference) {
    switch (preference) {
        case TransportPreference::MpegTs: return capabilities.mpeg_ts;
        case TransportPreference::Hls: return capabilities.hls;
    }
    return false;
}

const char* to_string(TransportPreference preference) {
    switch (preference) {
        case TransportPreference::MpegTs: return "mpeg-ts";
        case TransportPreference::Hls: return "hls";
    }
    return "unknown";
}

AccountMetadataResult parse_account_metadata(std::string_view body) {
    json root;
    try {
        root = json::parse(body);
    } catch (const json::exception&) {
        return {};
    }
    if (!root.is_object()) return {};

    AccountMetadataResult result;
    result.status = AccountMetadataStatus::Parsed;

    const auto user = root.find("user_info");
    if (user == root.end() || !user->is_object()) return {};

    if (const auto auth = user->find("auth"); auth != user->end()) {
        if (const auto authenticated = authentication_state(*auth);
            authenticated && !*authenticated) {
            result.status = AccountMetadataStatus::AuthenticationRejected;
            return result;
        }
    }

    if (const auto formats = user->find("allowed_output_formats"); formats != user->end()) {
        parse_transport_values(*formats, result.transports);
    }
    return result;
}

}  // namespace coax::xtream
