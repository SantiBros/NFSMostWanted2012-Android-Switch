#pragma once

namespace game::input {

// Register the MOGA controller Java classes (call before the game starts).
void register_java();
// Poll the platform controller/touch state and deliver changes to the game.
// Must run on the game's GL/main thread.
void pump();
// Debugging: turn the menu navigation layer off (all keys go to the game).
void set_navigation_enabled(bool on);
// Loader notices over the game (gyro steering state); after the frame is drawn.
void draw_overlay(int screen_w, int screen_h);

}  // namespace game::input
