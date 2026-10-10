// The port's debug menu: hold ZL + ZR and press Minus (in any order) to open
// it. It changes the loader's performance settings (frame-rate limit, vsync,
// 3D resolution, Switch clocks, the game's detail level) and the frame-rate
// counter. Settings are saved to settings.ini right away.
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
