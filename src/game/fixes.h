// Fixes for problems in the game's own code (see fixes.cpp).
#pragma once

namespace game::fixes {

// Patch libapp.so after relocation, before any game code runs.
void apply();
// Call once per frame on the main thread, outside the game's tick.
void frame();

}  // namespace game::fixes
