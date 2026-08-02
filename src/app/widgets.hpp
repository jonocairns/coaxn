#pragma once

#include <imgui.h>

// Controls built from the theme's palette and iconography.
//
// ImGui's stock widgets carry a frame, a label and a printed value. That is
// right for a form and wrong over video, where the chrome is louder than the
// thing it controls. These are the frameless equivalents. They live here
// rather than in the screen that first needed them so that the next surface
// inherits the same behaviour instead of growing its own, and so that App
// stays composition: what goes in the row, not how a control is drawn.
//
// Every one of them takes `fade`, the opacity of the surface it is on.
// Widgets that draw themselves cannot rely on the Alpha style variable — it
// reaches ImGui's own rendering, not draw-list calls — so the fade is passed
// down explicitly rather than read from a global.
namespace coax::app::widgets {

enum class Icon { Play, Pause, VolumeMuted, VolumeLow, VolumeHigh, Settings };

// A glyph with a hit box and no frame: hover and press show in the glyph and
// a wash behind it. `box` is the square it occupies, which is also what the
// layout should advance by. True on the frame it is clicked.
bool icon_button(const char* id, Icon icon, float box, float fade);

// A slim horizontal track, the whole of it `width` wide: the rule, and the
// room for the figure that appears at its right while the pointer is on it.
// `box` is the row height the track is centred in, so that a click anywhere
// near it lands rather than only on the three pixels of the rule itself.
//
// `unity` is marked along the track. Volume runs past 100 because mpv allows
// it, and the mark is what separates loud from boosted; pass 0 for a plain
// track. True when `value` changed this frame, by drag or by wheel.
bool volume_slider(const char* id, int& value, int maximum, int unity,
                   float width, float box, float fade);

}  // namespace coax::app::widgets
