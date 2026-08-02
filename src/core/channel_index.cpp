#include "core/channel_index.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace coax::core {
namespace {

std::string to_lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char ch : text) {
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

}  // namespace

void ChannelIndex::reset(std::vector<Category> categories, std::vector<Channel> channels) {
    categories_ = std::move(categories);
    channels_   = std::move(channels);

    channel_search_keys_.clear();
    channel_search_keys_.reserve(channels_.size());
    for (const auto& channel : channels_) {
        channel_search_keys_.push_back(to_lower(channel.name));
    }

    category_search_keys_.clear();
    category_search_keys_.reserve(categories_.size());
    for (const auto& category : categories_) {
        category_search_keys_.push_back(to_lower(category.name));
    }
}

std::vector<ChannelGroup> ChannelIndex::filtered(std::string_view query) const {
    const std::string needle = to_lower(query);

    // Category id -> index into the result, so channels land in provider order
    // without a lookup per channel.
    std::unordered_map<std::string_view, std::size_t> slot_of_category;
    std::vector<ChannelGroup>                         groups;
    groups.reserve(categories_.size());

    for (std::size_t i = 0; i < categories_.size(); ++i) {
        slot_of_category.emplace(std::string_view(categories_[i].id), groups.size());
        groups.push_back(ChannelGroup{&categories_[i], {}});
    }

    // Channels whose category the provider never declared still need a home.
    ChannelGroup ungrouped{nullptr, {}};

    for (std::size_t i = 0; i < channels_.size(); ++i) {
        const auto& channel = channels_[i];

        const auto slot = slot_of_category.find(std::string_view(channel.category_id));
        const bool has_category = slot != slot_of_category.end();

        if (!needle.empty()) {
            const bool name_match = channel_search_keys_[i].find(needle) != std::string::npos;
            const bool category_match =
                has_category && category_search_keys_[slot->second].find(needle) != std::string::npos;
            if (!name_match && !category_match) {
                continue;
            }
        }

        if (has_category) {
            groups[slot->second].channels.push_back(&channel);
        } else {
            ungrouped.channels.push_back(&channel);
        }
    }

    std::erase_if(groups, [](const ChannelGroup& group) { return group.channels.empty(); });
    if (!ungrouped.channels.empty()) {
        groups.push_back(std::move(ungrouped));
    }
    return groups;
}

const Channel* ChannelIndex::find(std::string_view channel_id) const {
    const auto it = std::ranges::find_if(
        channels_, [&](const Channel& c) { return c.id == channel_id; });
    return it == channels_.end() ? nullptr : &*it;
}

}  // namespace coax::core
