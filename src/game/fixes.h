// Fixes for problems in the game's own code (see fixes.cpp).
#pragma once

#include "core/common.h"

namespace game::fixes {

// Patch libapp.so after relocation, before any game code runs.
void apply();
// Call once per frame on the main thread, outside the game's tick.
void frame();
// Feature flags of the game's detail levels to add / take away (bit masks;
// see fixes.cpp). Set before the first frame; they apply once.
void set_feature_overrides(u32 add, u32 remove);

}  // namespace game::fixes
