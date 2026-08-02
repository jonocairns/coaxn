#pragma once

#include <imgui.h>

// The application's visual language: one palette, one scale, and the chrome
// the login screen paints by hand.
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
// ---------------------------------------------------------------------------

// Surfaces, back to front.
inline constexpr ImU32 kBackdropTop    = IM_COL32(0x16, 0x1D, 0x2B, 0xFF);
inline constexpr ImU32 kBackdropMiddle = IM_COL32(0x0A, 0x0E, 0x16, 0xFF);
inline constexpr ImU32 kBackdropBottom = IM_COL32(0x04, 0x06, 0x0A, 0xFF);
inline constexpr ImU32 kPanel          = IM_COL32(0x13, 0x18, 0x23, 0xFC);
inline constexpr ImU32 kPanelBorder    = IM_COL32(0x28, 0x32, 0x45, 0xFF);
inline constexpr ImU32 kTitleBar       = IM_COL32(0x0D, 0x11, 0x1A, 0xFF);
inline constexpr ImU32 kSeparator      = IM_COL32(0x30, 0x3B, 0x50, 0xFF);
inline constexpr ImU32 kTransparent    = IM_COL32(0x00, 0x00, 0x00, 0x00);
inline constexpr ImU32 kScrim          = IM_COL32(0x02, 0x04, 0x08, 0xB4);

// Text entry.
inline constexpr ImU32 kField        = IM_COL32(0x0B, 0x0F, 0x17, 0xFF);
inline constexpr ImU32 kFieldHover   = IM_COL32(0x14, 0x1B, 0x28, 0xFF);
inline constexpr ImU32 kFieldActive  = IM_COL32(0x16, 0x1E, 0x2D, 0xFF);

// Neutral controls. The accent is reserved for the primary action, so an
// ordinary button stays quiet.
inline constexpr ImU32 kControl       = IM_COL32(0x1D, 0x25, 0x33, 0xFF);
inline constexpr ImU32 kControlHover  = IM_COL32(0x28, 0x33, 0x45, 0xFF);
inline constexpr ImU32 kControlActive = IM_COL32(0x32, 0x3F, 0x55, 0xFF);
inline constexpr ImU32 kGrab          = IM_COL32(0x2C, 0x36, 0x49, 0xFF);
inline constexpr ImU32 kGrabHover     = IM_COL32(0x3A, 0x46, 0x5C, 0xFF);

// Selection. A channel list is hundreds of these at once, so they are washes
// over whatever is behind rather than fills of their own.
inline constexpr ImU32 kSelection      = IM_COL32(0xFF, 0xFF, 0xFF, 0x0F);
inline constexpr ImU32 kSelectionHover = IM_COL32(0xFF, 0xFF, 0xFF, 0x1C);

// Text.
inline constexpr ImU32 kText    = IM_COL32(0xE7, 0xEB, 0xF3, 0xFF);
inline constexpr ImU32 kTextDim = IM_COL32(0x82, 0x8E, 0xA5, 0xFF);
inline constexpr ImU32 kError   = IM_COL32(0xFF, 0x7B, 0x72, 0xFF);

// Accent.
inline constexpr ImU32 kAccent       = IM_COL32(0x35, 0x74, 0xF0, 0xFF);
inline constexpr ImU32 kAccentHover  = IM_COL32(0x4C, 0x8A, 0xFF, 0xFF);
inline constexpr ImU32 kAccentActive = IM_COL32(0x27, 0x5F, 0xD0, 0xFF);
inline constexpr ImU32 kOnAccent     = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);

// Installs the palette, the spacing and the UI font. Call once, after the
// ImGui context exists and before the backends are initialised.
void configure_style();

// Rescales the style for a display running at `scale` (1.0 at 96 DPI). Safe to
// call every time the window reports a change; repeats are ignored.
void set_dpi_scale(float scale);

// Paints the whole viewport: a vertical gradient with a soft accent glow
// behind the centre. Opaque, so it is only ever drawn when no video is on
// screen — the UI layer composites on top of the video visual, and an opaque
// fill would hide it.
void draw_backdrop();

// The coax mark: a connector seen end on. Drawn rather than loaded so it
// inherits the accent colour and costs no texture.
void draw_logo(ImDrawList* draw_list, ImVec2 centre, float radius);

}  // namespace coax::app::theme
