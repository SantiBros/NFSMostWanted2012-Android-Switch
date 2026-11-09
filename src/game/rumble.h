// Vibration (0.3.2). The Android game never vibrates, so the port makes the
// controller rumble with the game's own camera shakes: the game starts one for
// every crash (light, medium or hard), for takedowns and for nitro, with how
// strong it is and for how long (see rumble.cpp).
#pragma once

namespace game::rumble {

// Hook the game's camera shake start and stop (libapp.so, after relocation).
void install();
// Once per frame on the main thread: fades the active shakes and sends the
// result to the controller. `seconds`: time since the last frame.
void frame(float seconds);
// Stop now and stay quiet while `on` (debug menu open).
void hold(bool on);
// A short pulse at the chosen strength (when the setting changes).
void test_pulse();

}  // namespace game::rumble
