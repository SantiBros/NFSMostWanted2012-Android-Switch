// First-time setup on the console itself: the player only has to put their
// own APK (or .xapk bundle) and the OBB into the game folder.
//
// At start-up, when game/ is missing the libraries, the assets or the OBB:
//  1. files copied to the wrong place are moved where they belong (the
//     libraries still inside lib/armeabi-v7a/ or loose in game/, an extracted
//     assets folder one level too deep or its contents loose in game/, the OBB
//     in a subfolder or under another name);
//  2. what is still missing is taken out of the player's APK in game/ (or in
//     the port's folder): the 5 armeabi-v7a libraries and the assets/ folder.
//     An .xapk/.apks bundle works too, and the OBB inside it is copied out.
// Nothing is ever downloaded, and the player's APK/bundle is left as it is.
#pragma once

namespace game::setup {

// Returns true when it wrote files with the setup screen showing (the caller
// then restarts the port, see platform::finish_setup_screen), false when
// nothing was needed or could be done (the start-up checks then say what is
// missing). Stops with a message when the APK is unusable.
bool run();

}  // namespace game::setup
