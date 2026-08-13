#include "app/theme.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>

#include "util/log.hpp"

namespace coax::app::theme {
namespace {

// Tried in order, per face. Segoe UI ships with every supported Windows
// version; Selawik is the metric-compatible clone, and Arial is the belt and
// braces for a stripped install. Semibold rather than bold for the strong
// face: bold beside this body weight is a shout, and the point of the second
// face is hierarchy, not emphasis.
constexpr const char* kRegularFiles[]  = {"segoeui.ttf", "selawk.ttf", "arial.ttf"};
constexpr const char* kSemiboldFiles[] = {"seguisb.ttf", "selawksb.ttf", "segoeuib.ttf",
                                          "arialbd.ttf"};

// Extra advance baked into every glyph of the micro face. ImGui takes this in
// absolute pixels and does not scale it — with the type, the display, or the
// size a caller pushes — so it is set for the one size the face is used at.
// At micro sizes the error over a DPI change is a fraction of a pixel per
// glyph, which is why this is a constant rather than a reload.
constexpr float kMicroTracking = 0.9f;

// The display's contribution to the scale, and the unscaled style it is
// applied to. ScaleAllSizes truncates, so rescaling has to start from the
// original values every time rather than compounding on the current ones.
float      dpi_scale = 1.0f;
ImGuiStyle base_style;

// The type scale, in the order it is loaded. `regular` is the atlas's first
// font and therefore ImGui's default; the other two are only ever reached by
// pushing them.
ImFont* regular_face  = nullptr;
ImFont* strong_face   = nullptr;
ImFont* micro_face    = nullptr;

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

std::string fonts_directory() {
    char       directory[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryA(directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::string(directory) + "\\Fonts\\";
}

// The first of `files` that exists, at `size`, tracked out by `tracking`. Null
// when none of them do, which is a case every caller has an answer for rather
// than a failure: a stripped install still has to draw something.
ImFont* load_face(const std::string& directory, std::span<const char* const> files,
                  float size, float tracking) {
    if (directory.empty()) {
        return nullptr;
    }

    ImFontConfig config;
    // Without this a missing file asserts rather than returning null, and the
    // next candidate never gets its turn.
    config.Flags |= ImFontFlags_NoLoadError;
    config.GlyphExtraAdvanceX = tracking;

    for (const char* file : files) {
        const std::string path = directory + file;
        if (ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), size,
                                                                   &config)) {
            log::info("UI face: {}", path);
            return font;
        }
    }
    return nullptr;
}

void load_fonts() {
    const std::string directory = fonts_directory();

    // Loaded first, so it is Fonts[0] and therefore what ImGui draws with
    // unless something pushes otherwise.
    regular_face = load_face(directory, kRegularFiles, kFontSizeBase, 0.0f);
    if (regular_face == nullptr) {
        // The scalable default, not the classic bitmap one: that is only clean
        // at 13px, and everything here is drawn larger than that.
        ImFontConfig fallback;
        fallback.SizePixels = kFontSizeBase;
        regular_face        = ImGui::GetIO().Fonts->AddFontDefaultVector(&fallback);
        log::warn("No system UI font found; using the embedded default");
    }

    strong_face = load_face(directory, kSemiboldFiles, kFontSizeBase, 0.0f);
    if (strong_face == nullptr) {
        // A hierarchy of one weight is still a hierarchy — size and colour
        // carry it — and it beats refusing to start over a missing font file.
        strong_face = regular_face;
        log::warn("No semibold UI face found; headings fall back to the regular one");
    }

    // The semibold file again rather than a reference to the face above,
    // because the tracking belongs to the source: ImGui adds it to the glyph
    // advance as the atlas is baked, not as text is drawn.
    micro_face = load_face(directory, kSemiboldFiles, kMicroSize, kMicroTracking);
    if (micro_face == nullptr) {
        micro_face = load_face(directory, kRegularFiles, kMicroSize, kMicroTracking);
    }
    if (micro_face == nullptr) {
        micro_face = regular_face;
    }
}

}  // namespace

float ui_scale() {
    return kUiScaleBase * dpi_scale;
}

float scaled(float pixels) {
    return pixels * ui_scale();
}

ImU32 with_alpha(ImU32 color, float multiplier) {
    ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
    value.w *= std::clamp(multiplier, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(value);
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
    // One border in the whole application: the edge of a surface that floats
    // over something else. A field is already darker than the panel it is cut
    // into and a button already lighter, so an outline around either is a line
    // drawn to say what the fill has said.
    style.WindowBorderSize        = kStrokeHairline;
    style.PopupBorderSize         = kStrokeHairline;
    style.ChildBorderSize         = 0.0f;
    style.FrameBorderSize         = 0.0f;
    // Roomier than before, in both axes. Space is the cheapest way to separate
    // two things, and it is the one that leaves nothing on screen to look at.
    // Every one of these is a step on the scale, so the window's own rhythm and
    // the rhythm of anything laid out by hand inside it are the same rhythm.
    style.WindowPadding           = ImVec2(kSpace5, kSpace5);
    style.FramePadding            = ImVec2(kSpace3, kSpace2);
    style.ItemSpacing             = ImVec2(kSpace3, kSpace3);
    style.ItemInnerSpacing        = ImVec2(kSpace2, kSpace1);
    style.ScrollbarSize           = kSpace3;
    style.WindowTitleAlign        = ImVec2(0.0f, 0.5f);
    style.SeparatorTextBorderSize = kStrokeHairline;
    // No leading stub before the label. The default pads one in on the left,
    // which reads as an unexplained indent when the rule is faint.
    style.SeparatorTextPadding    = ImVec2(0.0f, kSpace3);
    style.SeparatorTextAlign      = ImVec2(0.0f, 0.5f);
    // Set rather than left at zero, which would make it whatever size the
    // first font in the atlas happens to have been loaded at. It is the same
    // number, but the type scale is meant to be stated, not inferred.
    style.FontSizeBase            = kFontSizeBase;

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

    load_fonts();
}

ScopedStyle::~ScopedStyle() {
    // The three stacks are independent, so the order between them is free; what
    // matters is that a guard pops only its own pushes. Nested guards are
    // destroyed in reverse order of construction, which keeps each one's
    // pushes contiguous at the top of the stack it touched.
    for (int pushed = 0; pushed < fonts_; ++pushed) {
        ImGui::PopFont();
    }
    if (vars_ > 0) {
        ImGui::PopStyleVar(vars_);
    }
    if (colors_ > 0) {
        ImGui::PopStyleColor(colors_);
    }
}

ScopedStyle& ScopedStyle::color(ImGuiCol target, ImU32 value) {
    ImGui::PushStyleColor(target, value);
    ++colors_;
    return *this;
}

ScopedStyle& ScopedStyle::var(ImGuiStyleVar target, float value) {
    ImGui::PushStyleVar(target, value);
    ++vars_;
    return *this;
}

ScopedStyle& ScopedStyle::var(ImGuiStyleVar target, ImVec2 value) {
    ImGui::PushStyleVar(target, value);
    ++vars_;
    return *this;
}

ScopedStyle& ScopedStyle::strong(float multiple) {
    push_strong(multiple);
    ++fonts_;
    return *this;
}

ScopedStyle& ScopedStyle::micro() {
    push_micro();
    ++fonts_;
    return *this;
}

void push_strong(float multiple) {
    // FontSizeBase, not GetFontSize(): the size handed to PushFont is scaled
    // by the global factors afterwards, so passing an already-scaled one
    // applies the display's scale twice.
    ImGui::PushFont(strong_face, ImGui::GetStyle().FontSizeBase * multiple);
}

void push_micro() {
    ImGui::PushFont(micro_face, kMicroSize);
}

void micro_label(const char* text) {
    ScopedStyle style;
    style.micro().color(ImGuiCol_Text, kTextFaint);
    ImGui::TextUnformatted(text);
}

void separator_label(const char* text) {
    ScopedStyle style;
    style.micro().color(ImGuiCol_Text, kTextFaint);
    ImGui::SeparatorText(text);
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

    // One value, no ramp. This was a gradient, and before that two bands with
    // an accent glow behind the login card; both were decoration on a surface
    // whose entire job is to be the thing the card is not. A ramp was also the
    // wrong tool at this end of the range: two or three levels of near-black
    // spread over a whole viewport gives each step tens of rows of pixels, so
    // it reads as bands with a fade between them rather than as a gradient.
    draw->AddRectFilled(top_left, bottom_right, kBackdrop);
}

void draw_overlay_scrim(ImDrawList* draw_list, ImVec2 top_left, ImVec2 bottom_right, float fade) {
    const ImU32 top    = with_alpha(kOverlayScrimTop, fade);
    const ImU32 bottom = with_alpha(kOverlayScrimBottom, fade);
    draw_list->AddRectFilledMultiColor(top_left, bottom_right, top, top, bottom, bottom);
}

namespace {

// Proportions of the mark, as fractions of its radius. scripts/make-icon.py
// carries the same numbers and the two are meant to agree: change one and the
// icon and the README's SVGs stop matching what the window draws.
constexpr float kFacetRadius = 0.95f;        // circumradius; the gap takes the rest
constexpr float kCutAngle    = 0.48869219f;  // 28 degrees
constexpr float kCutOffset   = 0.20f;        // how far the cut sits off centre
constexpr float kCutGap      = 0.11f;        // how far apart the pieces are pushed
constexpr int   kFacetSides  = 6;

// A convex polygon clipped by a half plane stays convex and gains at most one
// vertex, so seven is the most either piece can ever need.
constexpr int kMaxCorners = kFacetSides + 1;

// The part of `corners` on one side of the cut, written into `out`. Standard
// Sutherland-Hodgman: `keep` is +1 for the side the normal points at and -1
// for the other. Returns how many vertices were written.
int clip_to_cut(const ImVec2* corners, int count, ImVec2 normal, float distance,
                float keep, ImVec2* out) {
    const auto side = [&](ImVec2 point) {
        return keep * (normal.x * point.x + normal.y * point.y - distance);
    };
    const auto crossing = [](ImVec2 a, ImVec2 b, float was, float now) {
        const float t = was / (was - now);
        return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
    };

    int    written  = 0;
    ImVec2 previous = corners[count - 1];
    float  was      = side(previous);
    for (int corner = 0; corner < count; ++corner) {
        const ImVec2 point = corners[corner];
        const float  now   = side(point);
        if (now >= 0.0f) {
            if (was < 0.0f) {
                out[written++] = crossing(previous, point, was, now);
            }
            out[written++] = point;
        } else if (was >= 0.0f) {
            out[written++] = crossing(previous, point, was, now);
        }
        previous = point;
        was      = now;
    }
    return written;
}

// One piece of the cut hexagon, slid clear of the other along the cut's normal.
void draw_piece(ImDrawList* draw_list, const ImVec2* corners, ImVec2 normal,
                float distance, float keep, float shift, ImU32 color) {
    ImVec2    piece[kMaxCorners];
    const int written = clip_to_cut(corners, kFacetSides, normal, distance, keep, piece);
    for (int corner = 0; corner < written; ++corner) {
        piece[corner].x += normal.x * shift * keep;
        piece[corner].y += normal.y * shift * keep;
    }
    // Convex by construction, so this takes the cheap fill.
    draw_list->AddConvexPolyFilled(piece, written, color);
}

}  // namespace

void draw_logo(ImDrawList* draw_list, ImVec2 centre, float radius) {
    // A hexagon cut once and pulled apart. Pointy top rather than a circle:
    // every circular mark sits in the most crowded neighbourhood there is, and
    // the silhouette is the part that survives to a taskbar. The gap is what
    // makes it read as cut rather than as two colours of one shape.
    constexpr float kFirst = -1.57079633f;  // first vertex straight up
    constexpr float kStep  = 1.04719755f;   // 60 degrees

    ImVec2 corners[kFacetSides];
    for (int corner = 0; corner < kFacetSides; ++corner) {
        const float angle = kFirst + kStep * static_cast<float>(corner);
        corners[corner] = ImVec2(centre.x + std::cos(angle) * kFacetRadius * radius,
                                 centre.y + std::sin(angle) * kFacetRadius * radius);
    }

    const ImVec2 normal(std::sin(kCutAngle), -std::cos(kCutAngle));
    const float  distance =
        normal.x * centre.x + normal.y * centre.y + kCutOffset * radius;
    const float shift = kCutGap * radius * 0.5f;

    draw_piece(draw_list, corners, normal, distance, -1.0f, shift, kLogoBody);
    draw_piece(draw_list, corners, normal, distance, 1.0f, shift, kAccent);
}

void draw_play_icon(ImDrawList* draw_list, ImVec2 centre, float size, ImU32 color) {
    // Wider to the right than the base is to the left, which puts the mass of
    // the triangle over the centre it was handed rather than behind it.
    const float half = size * 0.5f;
    draw_list->AddTriangleFilled(ImVec2(centre.x - half * 0.50f, centre.y - half * 0.85f),
                                 ImVec2(centre.x - half * 0.50f, centre.y + half * 0.85f),
                                 ImVec2(centre.x + half * 0.90f, centre.y), color);
}

void draw_pause_icon(ImDrawList* draw_list, ImVec2 centre, float size, ImU32 color) {
    const float half = size * 0.5f;
    // Wider than a pause glyph usually is. Beside a line of text at the same
    // height, thin bars read as a stutter in the row rather than as a control.
    const float bar  = size * 0.26f;
    const float gap  = size * 0.20f;
    draw_list->AddRectFilled(ImVec2(centre.x - gap * 0.5f - bar, centre.y - half * 0.85f),
                             ImVec2(centre.x - gap * 0.5f, centre.y + half * 0.85f),
                             color, 0.0f);
    draw_list->AddRectFilled(ImVec2(centre.x + gap * 0.5f, centre.y - half * 0.85f),
                             ImVec2(centre.x + gap * 0.5f + bar, centre.y + half * 0.85f),
                             color, 0.0f);
}

void draw_stop_icon(ImDrawList* draw_list, ImVec2 centre, float size, ImU32 color) {
    const float half = size * 0.39f;
    draw_list->AddRectFilled(ImVec2(centre.x - half, centre.y - half),
                             ImVec2(centre.x + half, centre.y + half), color, 0.0f);
}

void draw_volume_icon(ImDrawList* draw_list, ImVec2 centre, float size, ImU32 color, int waves) {
    const float half      = size * 0.5f;
    const float thickness = std::max(size * 0.09f, 1.0f);

    // One polygon rather than a rectangle behind a triangle: the overlay fades
    // as a whole, and two fills at part opacity show the seam where they meet.
    const ImVec2 speaker[] = {
        ImVec2(centre.x - half * 0.95f, centre.y - half * 0.30f),
        ImVec2(centre.x - half * 0.45f, centre.y - half * 0.30f),
        ImVec2(centre.x + half * 0.05f, centre.y - half * 0.82f),
        ImVec2(centre.x + half * 0.05f, centre.y + half * 0.82f),
        ImVec2(centre.x - half * 0.45f, centre.y + half * 0.30f),
        ImVec2(centre.x - half * 0.95f, centre.y + half * 0.30f),
    };
    draw_list->AddConcavePolyFilled(speaker, IM_ARRAYSIZE(speaker), color);

    if (waves <= 0) {
        // The cross takes the room the waves would have had, so muted reads at
        // a glance rather than as a speaker with something small next to it.
        const float reach = half * 0.42f;
        const ImVec2 cross(centre.x + half * 0.62f, centre.y);
        draw_list->AddLine(ImVec2(cross.x - reach, cross.y - reach),
                           ImVec2(cross.x + reach, cross.y + reach), color, thickness);
        draw_list->AddLine(ImVec2(cross.x - reach, cross.y + reach),
                           ImVec2(cross.x + reach, cross.y - reach), color, thickness);
        return;
    }

    // Radians, measured from the cone's mouth: a little under a quarter turn
    // either side of horizontal.
    for (int wave = 0; wave < waves; ++wave) {
        draw_list->PathArcTo(ImVec2(centre.x + half * 0.05f, centre.y),
                             half * (0.45f + 0.33f * static_cast<float>(wave)), -0.85f, 0.85f);
        draw_list->PathStroke(color, thickness);
    }
}

void draw_settings_icon(ImDrawList* draw_list, ImVec2 centre, float size, ImU32 color) {
    const float half      = size * 0.5f;
    const float thickness = std::max(size * 0.085f, 1.0f);
    const float knob      = size * 0.15f;

    // Two sliders with their handles at different points along them. A gear at
    // this size is a smudge, and this reads as adjustment rather than as a
    // machine part. The rule stops either side of each handle instead of
    // running under it, for the same reason the speaker is one polygon.
    const float rows[2][2] = {{-half * 0.44f, -half * 0.24f}, {half * 0.44f, half * 0.34f}};
    for (const auto& row : rows) {
        const float y = centre.y + row[0];
        const float x = centre.x + row[1];
        draw_list->AddLine(ImVec2(centre.x - half * 0.92f, y), ImVec2(x - knob, y),
                           color, thickness);
        draw_list->AddLine(ImVec2(x + knob, y), ImVec2(centre.x + half * 0.92f, y),
                           color, thickness);
        draw_list->AddRectFilled(ImVec2(x - knob, y - knob), ImVec2(x + knob, y + knob),
                                 color, 0.0f);
    }
}

}  // namespace coax::app::theme
