#include "app/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app/theme.hpp"

namespace coax::app::widgets {
namespace {

// The glyph inside an icon button's hit box. Roughly half of it: the rest is
// the margin that makes a run of them read as separate controls without a
// frame around each one.
constexpr float kGlyphFraction = 0.52f;

void paint_icon(ImDrawList* draw_list, Icon icon, ImVec2 centre, float size, ImU32 color) {
    switch (icon) {
        case Icon::Play:        theme::draw_play_icon(draw_list, centre, size, color); break;
        case Icon::Pause:       theme::draw_pause_icon(draw_list, centre, size, color); break;
        case Icon::Stop:        theme::draw_stop_icon(draw_list, centre, size, color); break;
        case Icon::VolumeMuted: theme::draw_volume_icon(draw_list, centre, size, color, 0); break;
        case Icon::VolumeLow:   theme::draw_volume_icon(draw_list, centre, size, color, 1); break;
        case Icon::VolumeHigh:  theme::draw_volume_icon(draw_list, centre, size, color, 2); break;
        case Icon::Settings:    theme::draw_settings_icon(draw_list, centre, size, color); break;
    }
}

}  // namespace

bool icon_button(const char* id, Icon icon, float box, float fade) {
    ImDrawList*  draw_list = ImGui::GetWindowDrawList();
    const ImVec2 origin    = ImGui::GetCursorScreenPos();

    const bool pressed = ImGui::InvisibleButton(id, ImVec2(box, box));
    const bool active  = ImGui::IsItemActive();
    const bool hot     = active || ImGui::IsItemHovered();

    if (hot) {
        draw_list->AddRectFilled(
            origin, ImVec2(origin.x + box, origin.y + box),
            theme::with_alpha(active ? theme::kIconWashActive : theme::kIconWash, fade),
            0.0f);
    }

    paint_icon(draw_list, icon, ImVec2(origin.x + box * 0.5f, origin.y + box * 0.5f),
               box * kGlyphFraction, theme::with_alpha(hot ? theme::kIconHover : theme::kIcon, fade));

    return pressed;
}

bool volume_slider(const char* id, int& value, int maximum, int unity,
                   float width, float box, float fade) {
    if (maximum <= 0) {
        return false;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImGuiIO& io     = ImGui::GetIO();

    // The figure's slot is measured from the widest reading the track can
    // produce, so the rule does not shorten as the number gets longer.
    const float readout = ImGui::CalcTextSize("000%").x + theme::scaled(theme::kSpace2);
    const float track   = std::max(width - readout, theme::scaled(theme::kSpace6));

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  middle = origin.y + box * 0.5f;

    ImGui::InvisibleButton(id, ImVec2(track, box));
    const bool active = ImGui::IsItemActive();
    const bool hot    = active || ImGui::IsItemHovered();

    const int before = value;
    if (active) {
        const float fraction = std::clamp((io.MousePos.x - origin.x) / track, 0.0f, 1.0f);
        value = static_cast<int>(std::lround(fraction * static_cast<float>(maximum)));
    } else if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
        // Rounded away from zero so that a precision touchpad, which reports
        // the wheel in tenths, still moves the value by one notch per flick.
        value = std::clamp(value + static_cast<int>(std::lround(io.MouseWheel * 5.0f)),
                           0, maximum);
    }

    const float thickness = theme::scaled(theme::kStrokeTrack);
    const float top       = middle - thickness * 0.5f;
    const float bottom    = middle + thickness * 0.5f;
    const float filled    = track * (static_cast<float>(value) / static_cast<float>(maximum));

    draw_list->AddRectFilled(ImVec2(origin.x, top), ImVec2(origin.x + track, bottom),
                             theme::with_alpha(theme::kTrack, fade), 0.0f);
    if (filled > 0.0f) {
        draw_list->AddRectFilled(ImVec2(origin.x, top), ImVec2(origin.x + filled, bottom),
                                 theme::with_alpha(theme::kAccent, fade), 0.0f);
    }

    if (unity > 0 && unity < maximum) {
        const float at    = origin.x + track * (static_cast<float>(unity) /
                                                static_cast<float>(maximum));
        const float reach = thickness * 1.7f;
        draw_list->AddRectFilled(ImVec2(at, middle - reach),
                                 ImVec2(at + std::max(theme::scaled(theme::kStrokeHairline), 1.0f), middle + reach),
                                 theme::with_alpha(theme::kTrackMark, fade), 0.0f);
    }

    // Handle and figure only under the pointer. At rest the length of the fill
    // is the reading, and a number that never changes is noise over a picture.
    if (hot) {
        const float half_width  = theme::scaled(theme::kStrokeMarker);
        const float half_height = theme::scaled(theme::kSpace2);
        const float at = std::clamp(origin.x + filled, origin.x + half_width,
                                    origin.x + track - half_width);
        draw_list->AddRectFilled(ImVec2(at - half_width, middle - half_height),
                                 ImVec2(at + half_width, middle + half_height),
                                 theme::with_alpha(theme::kOnAccent, fade), 0.0f);

        char figure[8];
        std::snprintf(figure, sizeof(figure), "%d%%", value);
        draw_list->AddText(ImVec2(origin.x + track + theme::scaled(theme::kSpace2),
                                  middle - ImGui::GetTextLineHeight() * 0.5f),
                           theme::with_alpha(theme::kTextDim, fade), figure);
    }

    return value != before;
}

}  // namespace coax::app::widgets
