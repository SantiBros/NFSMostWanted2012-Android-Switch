// The port's debug menu: hold ZL + ZR and press Minus (in any order) to open
// it. Four pages, switched with L / R: PERFORMANCE (frame-rate limit, vsync,
// Switch clocks, the CPU boost while loading, ...), GRAPHICS (detail level,
// anti-aliasing, texture filtering, 3D resolution, frame-rate counter),
// CONTROLS & SOUND (tilt steering, sound volume) and INFORMATION (live
// figures: frame rate, clocks, temperatures, battery, threads, the last
// loading times). Settings are saved to settings.ini right away.
//
// While it is open the game gets no input; opening it during a race first
// pauses the race (the game's own pause, as with +).
#pragma once

#include "platform/platform.h"

namespace game::debug_menu {

// The button combination that opens (and closes) the menu.
bool combo_pressed(const platform::InputState& now, const platform::InputState& before);
bool is_open();
void open();
void close();
// Menu input while it is open (called once per frame with the raw controller state).
void update(const platform::InputState& now, const platform::InputState& before);
// Draw over the finished frame.
void draw(int screen_w, int screen_h);

}  // namespace game::debug_menu
