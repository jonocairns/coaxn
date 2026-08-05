#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/channel.hpp"

namespace coax::xtream {

struct Credentials {
    std::string base_url;   // scheme://host[:port], no trailing slash
    std::string username;
    std::string password;
};

struct Catalog {
    std::vector<core::Category> categories;
    std::vector<core::Channel>  channels;
};

// Blocking Xtream Codes client. Callers run it off the UI thread.
class Client {
public:
    explicit Client(Credentials creds) : creds_(std::move(creds)) {}

    // Loads live categories and channels, which doubles as credential
    // verification: bad credentials come back as a provider error.
    bool fetch_catalog(Catalog& out, std::string& error) const;

    // Builds the authenticated playback URL. This value is handed straight to
    // libmpv and must never reach the UI, the log or a diagnostics export.
    [[nodiscard]] std::string stream_url(const core::Channel& channel) const;

    [[nodiscard]] const Credentials& credentials() const { return creds_; }

private:
    bool get(std::string_view query, std::string& body, std::string& error) const;

    Credentials creds_;
};

}  // namespace coax::xtream
