// The game's icon on the player's own copy of the port (0.3.2, issue #3).
//
// The released .nro has the port's own plain icon: no EA artwork may be
// shipped. At start-up the port takes the game's launcher icon out of the
// player's APK (kept as game/icon.jpg, so it survives deleting the APK and
// updating the port) and writes it, with the title "NFS Most Wanted 2012",
// into the .nro that is running. The Homebrew Menu then shows the game's icon,
// and a forwarder installed from the .nro afterwards has it too.
// tools/prepare_sd.py --nro does the same on a computer.
#pragma once

namespace game::icon {

// Before the game starts (the .nro's RomFS is briefly unmounted on the Switch).
// NFSMW_TEST_NRO=<file> (desktop tests): patch that .nro instead.
void update();

}  // namespace game::icon
