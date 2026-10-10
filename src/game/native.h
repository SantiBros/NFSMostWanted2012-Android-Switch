// Native (host) versions of hot spots in the game's own code (see native.cpp).
#pragma once

namespace game::native {

// Patch libapp.so after relocation, before any game code runs.
// NFSMW_NO_NATIVE=1 leaves the game's code as it is (for comparisons).
void apply();

}  // namespace game::native
