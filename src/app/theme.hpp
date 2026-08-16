#pragma once

#include <imgui.h>

// The application's visual language: one palette, one type scale, and the
// chrome the login screen paints by hand.
namespace coax::app::theme {

// The application's own sizing factor, on top of whatever the display is
// running at. One means desktop sizing, which is how this is used: at a
// keyboard, not from across a room. Raise it — 1.45 was the old value — if it
// ever moves to a television, and everything follows, because nothing here is
// allowed a scale of its own.
inline constexpr float kUiScaleBase = 1.0f;

// The factor every hand-written pixel size has to pass through: the taste
// factor above multiplied by the scale of the monitor the window is on. Not a
// constant, because the second half of that is only known at runtime and
// changes when the window is dragged between displays.
[[nodiscard]] float ui_scale();
[[nodiscard]] float scaled(float pixels);

// ---------------------------------------------------------------------------
// The palette. Every colour the application draws is named here — including
// the ones only the ImGui style table consumes — so that a colour is changed
// in one place rather than hunted through the draw code.
//
// Neutral by construction. The surfaces are greys with no hue of their own, so
// the accent is the only colour in the window and does not have to shout to be
// found. Anything tinted here would be competing with the one thing that is
// supposed to mean something.
// ---------------------------------------------------------------------------

// Surfaces, back to front. The backdrop is one flat value: a ramp across a
// couple of levels of near-black has too few steps to be smooth, and an
// 8-bit gradient over that little range bands into visible strips rather than
// fading.
inline constexpr ImU32 kBackdrop       = IM_COL32(0x0B, 0x0B, 0x0D, 0xFF);
inline constexpr ImU32 kPanel          = IM_COL32(0x11, 0x11, 0x13, 0xFA);
inline constexpr ImU32 kPanelBorder    = IM_COL32(0x1F, 0x1F, 0x23, 0xFF);
inline constexpr ImU32 kTitleBar       = IM_COL32(0x0C, 0x0C, 0x0E, 0xFF);
inline constexpr ImU32 kSeparator      = IM_COL32(0x1C, 0x1C, 0x20, 0xFF);
inline constexpr ImU32 kTransparent    = IM_COL32(0x00, 0x00, 0x00, 0x00);
inline constexpr ImU32 kScrim          = IM_COL32(0x00, 0x00, 0x00, 0xB0);

// Text entry. Darker than the panel rather than lighter: a field is a well cut
// into the surface, which needs no border to read as one.
inline constexpr ImU32 kField        = IM_COL32(0x0A, 0x0A, 0x0C, 0xFF);
inline constexpr ImU32 kFieldHover   = IM_COL32(0x10, 0x10, 0x13, 0xFF);
inline constexpr ImU32 kFieldActive  = IM_COL32(0x13, 0x13, 0x17, 0xFF);

// Neutral controls. The accent is reserved for the primary action, so an
// ordinary button stays quiet.
inline constexpr ImU32 kControl       = IM_COL32(0x17, 0x17, 0x1A, 0xFF);
inline constexpr ImU32 kControlHover  = IM_COL32(0x20, 0x20, 0x25, 0xFF);
inline constexpr ImU32 kControlActive = IM_COL32(0x2A, 0x2A, 0x30, 0xFF);
inline constexpr ImU32 kGrab          = IM_COL32(0x26, 0x26, 0x2B, 0xFF);
inline constexpr ImU32 kGrabHover     = IM_COL32(0x34, 0x34, 0x3B, 0xFF);

// Selection. A channel list is hundreds of these at once, so they are washes
// over whatever is behind rather than fills of their own.
inline constexpr ImU32 kSelection      = IM_COL32(0xFF, 0xFF, 0xFF, 0x0D);
inline constexpr ImU32 kSelectionHover = IM_COL32(0xFF, 0xFF, 0xFF, 0x17);

// Text, in three weights of attention: the reading, the label beside it, and
// the heading above them both. Faint is deliberately quiet — it is only ever
// used at micro size, where the tracking does the work the colour would.
// Every one of these is at or above 4.5:1 on the surface it is used on, which
// is what normal-size text needs to be legible to normal-size eyesight. Faint
// is the floor of the system, not a licence to go quieter: it was #5C5C64 and
// measured 2.85:1 at eleven pixels, which is a colour chosen by eye on a good
// monitor and not readable on a bad one.
inline constexpr ImU32 kText      = IM_COL32(0xE9, 0xE9, 0xEC, 0xFF);  // 15.6:1
inline constexpr ImU32 kTextDim   = IM_COL32(0x8B, 0x8B, 0x93, 0xFF);  //  5.6:1
inline constexpr ImU32 kTextFaint = IM_COL32(0x7C, 0x7C, 0x84, 0xFF);  //  4.6:1
inline constexpr ImU32 kError     = IM_COL32(0xEC, 0x6A, 0x62, 0xFF);  //  6.1:1

// Accent. This is the brand colour and it is only ever decoration — the mark,
// the volume fill, the rule beside the playing channel.
//
// Its lightness is not a taste decision. The mark has no ground of its own and
// the accent is the one value that cannot differ between the two SVGs, so it
// has to read on near-black and on white at once. Sweeping every hue at every
// usable saturation, the best worst-case contrast any colour can reach against
// both grounds is 4.43:1 — and every hue reaches it. So the constraint pins the
// lightness and leaves the hue free; this one sits on that balance point at
// 4.43:1 on the backdrop and 4.44:1 on white. The blue it replaced was at
// 3.85:1 against white, which is why the mark used to go soft on a light page.
inline constexpr ImU32 kAccent      = IM_COL32(0x63, 0x63, 0xFD, 0xFF);
inline constexpr ImU32 kAccentHover = IM_COL32(0x90, 0x90, 0xFD, 0xFF);  // 7.1:1

// The primary action, which is the one place the accent has a label on it. A
// step darker than the brand, because white on kAccent is 4.43:1 and normal
// text needs 4.5. That step is now barely visible — sitting the accent on the
// balance point already puts it within a hair of carrying a white label — but
// it still has to exist. Hover and press go down the ramp rather than up: a
// lighter fill would take the label back under, so this button deepens under
// the pointer instead of brightening.
inline constexpr ImU32 kAccentFill       = IM_COL32(0x61, 0x61, 0xFD, 0xFF);  // 4.5:1
inline constexpr ImU32 kAccentFillHover  = IM_COL32(0x4D, 0x4D, 0xFC, 0xFF);  // 5.6:1
inline constexpr ImU32 kAccentFillActive = IM_COL32(0x2C, 0x2C, 0xFC, 0xFF);  // 7.3:1
inline constexpr ImU32 kOnAccent         = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);

// The mark's body — the large piece of the cut hexagon. Its counterpart is the
// accent offcut, so this is the only other colour the application draws: a
// light slate rather than the dark one the mark was designed against, because
// here it is seen on near-black and a dark body on a dark ground is most of a
// missing logo.
inline constexpr ImU32 kLogoBody = IM_COL32(0xA9, 0xB3, 0xC9, 0xFF);  // 9.3:1

// The playback overlay. It sits over the picture rather than over a surface of
// its own, so it is a ramp into darkness at the bottom of the frame rather
// than a panel: legible over a bright shot without drawing an edge across it.
inline constexpr ImU32 kOverlayScrimTop    = IM_COL32(0x00, 0x00, 0x00, 0x00);
inline constexpr ImU32 kOverlayScrimBottom = IM_COL32(0x00, 0x00, 0x00, 0xD9);

// Frameless controls. An icon carries its own state — brighter under the
// pointer, over a wash while it is held — because a filled button behind every
// glyph is more chrome than the row it controls.
inline constexpr ImU32 kIcon           = IM_COL32(0xB4, 0xB4, 0xBC, 0xFF);
inline constexpr ImU32 kIconHover      = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
inline constexpr ImU32 kIconWash       = IM_COL32(0xFF, 0xFF, 0xFF, 0x0F);
inline constexpr ImU32 kIconWashActive = IM_COL32(0xFF, 0xFF, 0xFF, 0x1F);
inline constexpr ImU32 kTrack          = IM_COL32(0xFF, 0xFF, 0xFF, 0x26);
inline constexpr ImU32 kTrackMark      = IM_COL32(0xFF, 0xFF, 0xFF, 0x40);

// The same colour at a fraction of its own opacity. Anything drawn straight
// into a draw list has to fade itself: the Alpha style variable only reaches
// widgets, and the overlay is half hand-drawn.
[[nodiscard]] ImU32 with_alpha(ImU32 color, float multiplier);

// ---------------------------------------------------------------------------
// The spacing scale. Every gap, inset and margin in the application is one of
// these, in design pixels, before ui_scale is applied.
//
// A grid rather than a list of the numbers that were already there. Colour has
// had one source of truth from the start and spacing did not: it was two dozen
// literals spread through the draw code, so a rhythm nobody could see was the
// sum of choices nobody could find. Steps of four, because that is the
// smallest interval that still looks deliberate once the display scale has
// multiplied it.
// ---------------------------------------------------------------------------

inline constexpr float kSpace1 = 4.0f;
inline constexpr float kSpace2 = 8.0f;
inline constexpr float kSpace3 = 12.0f;
inline constexpr float kSpace4 = 16.0f;
inline constexpr float kSpace5 = 20.0f;
inline constexpr float kSpace6 = 24.0f;
inline constexpr float kSpace7 = 32.0f;
inline constexpr float kSpace8 = 48.0f;

// Stroke weights, which are not spacing: a rule is as thick as it needs to be
// to survive being drawn, and rounding one to the nearest four would make it a
// bar. Kept here anyway so that nothing in the draw code is a bare number.
inline constexpr float kStrokeHairline = 1.0f;
inline constexpr float kStrokeMarker   = 2.0f;
inline constexpr float kStrokeTrack    = 3.0f;

// ---------------------------------------------------------------------------
// Scoped style pushes.
//
// ImGui unwinds its style stacks by count, so a Pop that disagrees with its
// Push does not fail where it was written — it corrupts every surface drawn
// afterwards. A counted pop also has to be kept in step by hand through every
// later edit, and a return between a push and its pop leaks in silence. This
// makes the pairing structural instead: the destructor pops exactly what was
// pushed, whichever way the function is left.
// ---------------------------------------------------------------------------
class ScopedStyle {
public:
    ScopedStyle() = default;
    ~ScopedStyle();

    ScopedStyle(const ScopedStyle&)            = delete;
    ScopedStyle& operator=(const ScopedStyle&) = delete;

    ScopedStyle& color(ImGuiCol target, ImU32 value);
    ScopedStyle& var(ImGuiStyleVar target, float value);
    ScopedStyle& var(ImGuiStyleVar target, ImVec2 value);
    // The type scale, pushed under the same guard as everything else.
    ScopedStyle& strong(float multiple = 1.0f);
    ScopedStyle& micro();

private:
    int colors_ = 0;
    int vars_   = 0;
    int fonts_  = 0;
};

// Installs the palette, the spacing and the type scale. Call once, after the
// ImGui context exists and before the backends are initialised.
void configure_style();

// Rescales the style for a display running at `scale` (1.0 at 96 DPI). Safe to
// call every time the window reports a change; repeats are ignored.
void set_dpi_scale(float scale);

// ---------------------------------------------------------------------------
// The type scale. Three faces, not one face at several sizes: weight is what
// separates a heading from the thing it heads, and a single regular face
// scaled up only ever produces large body text.
//
// A face is loaded per style rather than parameterised because the one thing
// that cannot be set per draw call is tracking — ImGui bakes it into the
// glyph advance at load time.
// ---------------------------------------------------------------------------

// The design size of body text, before any scaling.
inline constexpr float kFontSizeBase = 15.0f;
// And of the micro label, which is small enough that it is a texture rather
// than something anyone reads twice.
inline constexpr float kMicroSize = 11.0f;

// The regular face is the atlas default, so it needs no accessor: it is what
// is drawn with unless one of these pushes something else. Both pair with
// ImGui::PopFont().
//
// The semibold face, at `multiple` of the current base text size, for the one
// thing on a surface that should be read first. Falls back to the regular face
// where Windows has no semibold to give.
void push_strong(float multiple = 1.0f);
// Semibold again, tracked out and at micro size, for uppercase labels.
void push_micro();

// A section heading: uppercase, tracked, faint, and small enough that it never
// competes with what it introduces. Pass the text already uppercased — this
// styles a label, it does not rewrite one. The `separator_` form draws the
// same label with the rule ImGui::SeparatorText puts beside it.
void micro_label(const char* text);
void separator_label(const char* text);

// ---------------------------------------------------------------------------
// Hand-drawn chrome.
// ---------------------------------------------------------------------------

// Paints the whole viewport in one flat near-black. Opaque, so it is only ever
// drawn when no video is on screen — the UI layer composites on top of the
// video visual, and an opaque fill would hide it.
void draw_backdrop();

// The ramp the playback overlay sits on: nothing at the top edge, near-black
// at the bottom. Transparent, unlike the backdrop, because there is video
// behind it — it buys legibility for the row without hiding the picture.
void draw_overlay_scrim(ImDrawList* draw_list, ImVec2 top_left, ImVec2 bottom_right, float fade);

// The same ramp the other way up: solid along the top edge, nothing at the
// bottom. What the title strip sits on, where the edge being reached for is the
// one above rather than the one below.
void draw_title_scrim(ImDrawList* draw_list, ImVec2 top_left, ImVec2 bottom_right, float fade);

// The coax mark: a hexagon cut once, the offcut pushed clear of the body and
// given the accent. Drawn rather than loaded so it takes the palette's own
// colours and costs no texture. It has no tile of its own; it sits on whatever
// surface it is placed on. `radius` is the mark's outer edge, which the lowest
// vertex plus half the gap reaches almost exactly.
void draw_logo(ImDrawList* draw_list, ImVec2 centre, float radius);

// ---------------------------------------------------------------------------
// The overlay's iconography, drawn for the same reasons as the mark. The UI
// font is whatever the system happens to provide and cannot be relied on for
// symbols, and a drawn glyph takes the colour and the fade it is handed.
// `size` is the box each one fills, `centre` its middle.
// ---------------------------------------------------------------------------

void draw_play_icon(ImDrawList* draw_list, ImVec2 centre, float size, ImU32 color);
void draw_pause_icon(ImDrawList* draw_list, ImVec2 centre, float size, ImU32 color);
void draw_stop_icon(ImDrawList* draw_list, ImVec2 centre, float size, ImU32 color);
// `waves` is how many arcs stand beside the cone; zero draws the muted cross.
void draw_volume_icon(ImDrawList* draw_list, ImVec2 centre, float size, ImU32 color, int waves);
void draw_settings_icon(ImDrawList* draw_list, ImVec2 centre, float size, ImU32 color);

// The window controls. Plain geometry at the sizes Windows itself uses for
// these, because they are the one place in the application where being
// recognised instantly matters more than having a voice of its own.
void draw_minimise_icon(ImDrawList* draw_list, ImVec2 centre, float size, ImU32 color);
void draw_close_icon(ImDrawList* draw_list, ImVec2 centre, float size, ImU32 color);
// Four corner brackets, pointing out. `collapse` turns them inward, for the
// same control while the window is already fullscreen.
void draw_fullscreen_icon(ImDrawList* draw_list, ImVec2 centre, float size, ImU32 color,
                          bool collapse);

}  // namespace coax::app::theme
