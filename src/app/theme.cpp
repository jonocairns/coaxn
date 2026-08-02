#include "app/theme.hpp"

#include <windows.h>

#include <algorithm>
#include <string>

#include "util/log.hpp"

namespace coax::app::theme {
namespace {

// Design size, before any scaling. Since 1.92 ImGui rasterises glyphs at the
// scaled size rather than stretching an atlas, so this stays a design number
// and the text is sharp whatever the scale ends up being.
constexpr float kFontSizeBase = 16.0f;

// Tried in order. Segoe UI ships with every supported Windows version; Arial
// is the belt and braces for a stripped install.
constexpr const char* kFontFiles[] = {"segoeui.ttf", "arial.ttf"};

// The display's contribution to the scale, and the unscaled style it is
// applied to. ScaleAllSizes truncates, so rescaling has to start from the
// original values every time rather than compounding on the current ones.
float      dpi_scale = 1.0f;
ImGuiStyle base_style;

ImVec4 rgba(ImU32 color) {
    return ImGui::ColorConvertU32ToFloat4(color);
}

// Same hue, different alpha. Keeps a derived colour honestly related to the
// one it comes from instead of hand-picked and slightly off.
ImU32 fade(ImU32 color, float alpha) {
    ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
    value.w      = alpha;
    return ImGui::ColorConvertFloat4ToU32(value);
}

void apply_scale() {
    ImGuiStyle& style = ImGui::GetStyle();
    style             = base_style;
    style.ScaleAllSizes(ui_scale());
    // Spacing is scaled above; fonts are scaled by the font system, and it
    // wants the two factors separately so the display's share can change
    // without disturbing the application's own.
    style.FontScaleMain = kUiScaleBase;
    style.FontScaleDpi  = dpi_scale;
}

void load_font() {
    ImFontConfig config;
    // Without this a missing file asserts rather than returning null, and the
    // fallback below never gets its turn.
    config.Flags |= ImFontFlags_NoLoadError;

    char       directory[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryA(directory, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        for (const char* file : kFontFiles) {
            const std::string path = std::string(directory) + "\\Fonts\\" + file;
            if (ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), kFontSizeBase, &config)) {
                log::info("UI font: {}", path);
                return;
            }
        }
    }

    // The scalable default, not the classic bitmap one: that is only clean at
    // 13px, and everything here is drawn larger than that.
    ImFontConfig fallback;
    fallback.SizePixels = kFontSizeBase;
    ImGui::GetIO().Fonts->AddFontDefaultVector(&fallback);
    log::warn("No system UI font found; using the embedded default");
}

}  // namespace

float ui_scale() {
    return kUiScaleBase * dpi_scale;
}

float scaled(float pixels) {
    return pixels * ui_scale();
}

void configure_style() {
    ImGuiStyle& style = ImGui::GetStyle();

    // Square throughout. Every corner radius is zero on purpose, so anything
    // drawn by hand has to pass 0.0f rounding to match.
    style.WindowRounding          = 0.0f;
    style.ChildRounding           = 0.0f;
    style.FrameRounding           = 0.0f;
    style.PopupRounding           = 0.0f;
    style.GrabRounding            = 0.0f;
    style.ScrollbarRounding       = 0.0f;
    style.WindowBorderSize        = 1.0f;
    style.FrameBorderSize         = 1.0f;
    style.WindowPadding           = ImVec2(16.0f, 14.0f);
    style.FramePadding            = ImVec2(12.0f, 8.0f);
    style.ItemSpacing             = ImVec2(10.0f, 9.0f);
    style.ItemInnerSpacing        = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize           = 12.0f;
    style.WindowTitleAlign        = ImVec2(0.0f, 0.5f);
    style.SeparatorTextBorderSize = 1.0f;
    // No leading stub before the label. The default pads one in on the left,
    // which reads as an unexplained indent when the rule is faint.
    style.SeparatorTextPadding    = ImVec2(0.0f, 8.0f);
    style.SeparatorTextAlign      = ImVec2(0.0f, 0.5f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                 = rgba(kText);
    colors[ImGuiCol_TextDisabled]         = rgba(kTextDim);
    colors[ImGuiCol_WindowBg]             = rgba(kPanel);
    colors[ImGuiCol_ChildBg]              = rgba(kTransparent);
    colors[ImGuiCol_PopupBg]              = rgba(kPanel);
    colors[ImGuiCol_Border]               = rgba(kPanelBorder);
    colors[ImGuiCol_BorderShadow]         = rgba(kTransparent);
    colors[ImGuiCol_FrameBg]              = rgba(kField);
    colors[ImGuiCol_FrameBgHovered]       = rgba(kFieldHover);
    colors[ImGuiCol_FrameBgActive]        = rgba(kFieldActive);
    colors[ImGuiCol_TitleBg]              = rgba(kTitleBar);
    colors[ImGuiCol_TitleBgActive]        = rgba(kTitleBar);
    colors[ImGuiCol_TitleBgCollapsed]     = rgba(fade(kTitleBar, 0.75f));
    colors[ImGuiCol_MenuBarBg]            = rgba(kTitleBar);
    colors[ImGuiCol_ScrollbarBg]          = rgba(kTransparent);
    colors[ImGuiCol_ScrollbarGrab]        = rgba(kGrab);
    colors[ImGuiCol_ScrollbarGrabHovered] = rgba(kGrabHover);
    colors[ImGuiCol_ScrollbarGrabActive]  = rgba(kAccent);
    colors[ImGuiCol_CheckMark]            = rgba(kAccentHover);
    colors[ImGuiCol_SliderGrab]           = rgba(kAccent);
    colors[ImGuiCol_SliderGrabActive]     = rgba(kAccentHover);
    colors[ImGuiCol_Button]               = rgba(kControl);
    colors[ImGuiCol_ButtonHovered]        = rgba(kControlHover);
    colors[ImGuiCol_ButtonActive]         = rgba(kControlActive);
    colors[ImGuiCol_Header]               = rgba(kSelection);
    colors[ImGuiCol_HeaderHovered]        = rgba(kSelectionHover);
    colors[ImGuiCol_HeaderActive]         = rgba(fade(kAccent, 0.55f));
    colors[ImGuiCol_Separator]            = rgba(kSeparator);
    colors[ImGuiCol_SeparatorHovered]     = rgba(fade(kAccent, 0.60f));
    colors[ImGuiCol_SeparatorActive]      = rgba(kAccent);
    colors[ImGuiCol_ResizeGrip]           = rgba(kSelection);
    colors[ImGuiCol_ResizeGripHovered]    = rgba(fade(kAccent, 0.60f));
    colors[ImGuiCol_ResizeGripActive]     = rgba(kAccent);
    colors[ImGuiCol_InputTextCursor]      = rgba(kAccentHover);
    colors[ImGuiCol_TextSelectedBg]       = rgba(fade(kAccent, 0.35f));
    colors[ImGuiCol_TextLink]             = rgba(kAccentHover);
    colors[ImGuiCol_NavCursor]            = rgba(kAccentHover);
    colors[ImGuiCol_ModalWindowDimBg]     = rgba(kScrim);

    // Everything above is at scale 1. The snapshot is what every later
    // rescale starts from.
    base_style = style;
    apply_scale();

    load_font();
}

void set_dpi_scale(float scale) {
    // A display cannot plausibly be outside this, and a bad value would be
    // baked into every size until the next change.
    scale = std::clamp(scale, 0.5f, 4.0f);
    if (scale == dpi_scale) {
        return;
    }
    dpi_scale = scale;
    apply_scale();
    log::info("Display scale {:.2f}x (UI at {:.2f}x)", scale, ui_scale());
}

void draw_backdrop() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList*          draw     = ImGui::GetBackgroundDrawList();

    const ImVec2 top_left(viewport->Pos.x, viewport->Pos.y);
    const ImVec2 bottom_right(viewport->Pos.x + viewport->Size.x,
                              viewport->Pos.y + viewport->Size.y);

    // Two stacked bands rather than one: a single linear ramp over the whole
    // height reads as flat, and the midpoint gives the falloff a shoulder.
    const float middle = viewport->Pos.y + viewport->Size.y * 0.55f;
    draw->AddRectFilledMultiColor(top_left, ImVec2(bottom_right.x, middle),
                                  kBackdropTop, kBackdropTop,
                                  kBackdropMiddle, kBackdropMiddle);
    draw->AddRectFilledMultiColor(ImVec2(top_left.x, middle), bottom_right,
                                  kBackdropMiddle, kBackdropMiddle,
                                  kBackdropBottom, kBackdropBottom);

    // A soft accent glow behind where the card sits. Concentric discs of a
    // low constant alpha accumulate into a smooth falloff, which is cheaper
    // and sharper than a blurred texture would be.
    constexpr int kRings = 26;
    const ImVec2  centre(viewport->Pos.x + viewport->Size.x * 0.5f,
                         viewport->Pos.y + viewport->Size.y * 0.42f);
    const float   radius = std::max(viewport->Size.x, viewport->Size.y) * 0.62f;
    const ImU32   glow   = fade(kAccent, 4.0f / 255.0f);
    for (int ring = kRings; ring > 0; --ring) {
        draw->AddCircleFilled(centre, radius * (static_cast<float>(ring) / kRings), glow, 64);
    }
}

void draw_logo(ImDrawList* draw_list, ImVec2 centre, float radius) {
    draw_list->AddCircleFilled(centre, radius, kBackdropMiddle, 48);
    draw_list->AddCircle(centre, radius, kAccent, 48, radius * 0.16f);
    draw_list->AddCircle(centre, radius * 0.58f, fade(kAccent, 0.55f), 40, radius * 0.10f);
    draw_list->AddCircleFilled(centre, radius * 0.20f, kAccentHover, 24);
}

}  // namespace coax::app::theme
