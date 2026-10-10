// Gyro (motion) steering.
//
// The game only lets you choose its own "tilt to steer" mode when no gamepad
// is connected, and that mode drops the gamepad controls. So the loader does
// tilt steering itself: while driving, the tilt of the console or controller
// is turned into the steering value the game reads from the MOGA left stick.
// Every button keeps its function, and moving the left stick still steers
// (the stick wins over tilt while it is pushed).
//
// It is switched on/off in the game's Options > Controls screen: the "TILT TO
// STEER" tile toggles it (that tile otherwise only says "disconnect your MOGA
// controller"). The choice is saved in settings.ini (in the port's folder), together
// with the sensitivity and an invert switch.
#pragma once

#include "platform/platform.h"

namespace game::motion {

void load_settings();
bool enabled();
void set_enabled(bool on);  // saves the setting
bool sensor_seen();         // a motion sensor has reported data

// Steering from tilt, -1 (full left) .. +1 (full right); call once per frame.
// Returns false when tilt steering is off or there is no sensor data.
bool steering(const platform::InputState& s, float& out);
// Forget the filter state (e.g. at the start of a race).
void reset();

}  // namespace game::motion
