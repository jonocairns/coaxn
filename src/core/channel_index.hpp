#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/channel.hpp"

namespace coax::core {

// A category together with the channels inside it that survived the filter.
// Pointers reference storage owned by the ChannelIndex and stay valid until
// the next reset().
struct ChannelGroup {
    const Category*             category = nullptr;
    std::vector<const Channel*> channels;
};

// Owns the full channel and category set and derives filtered, grouped views
// of it. Deliberately free of Windows, UI and provider types: this is the part
// of the application a second platform would reuse unchanged.
class ChannelIndex {
public:
    void reset(std::vector<Category> categories, std::vector<Channel> channels);

    // Case-insensitive substring match against channel and category names.
    // An empty query matches everything. Provider category order is preserved
    // and groups left empty by the filter are dropped.
    [[nodiscard]] std::vector<ChannelGroup> filtered(std::string_view query) const;

    [[nodiscard]] const Channel* find(std::string_view channel_id) const;

    [[nodiscard]] std::size_t channel_count()  const { return channels_.size(); }
    [[nodiscard]] std::size_t category_count() const { return categories_.size(); }
    [[nodiscard]] bool        empty()          const { return channels_.empty(); }

private:
    std::vector<Category>    categories_;
    std::vector<Channel>     channels_;

    // Lowercased names, parallel to the vectors above, so filtering on every
    // keystroke does not re-lowercase thousands of strings.
    std::vector<std::string> channel_search_keys_;
    std::vector<std::string> category_search_keys_;
};

}  // namespace coax::core
