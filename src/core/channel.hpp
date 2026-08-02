#pragma once

#include <string>

namespace coax::core {

struct Category {
    std::string id;
    std::string name;
};

// A live channel as the application understands it, independent of the
// provider it came from. The UI only ever handles these; authenticated
// playback URLs are built at the last moment by the provider client.
struct Channel {
    std::string id;           // provider stream id, kept as text
    std::string name;
    std::string category_id;
    std::string logo_url;
    int         number = 0;   // provider-assigned display order
};

}  // namespace coax::core
