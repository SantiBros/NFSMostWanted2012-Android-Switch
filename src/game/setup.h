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

#include <string>

#include "core/common.h"

namespace game::setup {

// Returns true when it wrote files with the setup screen showing (the caller
// then restarts the port, see platform::finish_setup_screen), false when
// nothing was needed or could be done (the start-up checks then say what is
// missing). Stops with a message when the APK is unusable.
bool run();

// A game library as loaded: the build the port is made for (Tested: 1.3.128
// as the port was tested with), or else as the setup took it out of the
// player's APK (AsExtracted: e.g. a modified APK), changed since then
// (Changed: an SD card error, most likely; *recorded_crc says what it was), or
// not known (Unknown: copied by hand).
enum class LibraryCheck { Tested, AsExtracted, Changed, Unknown };
LibraryCheck check_library(const std::string& name, u64 size, u32 crc, u32* recorded_crc = nullptr);

// Unpack a zip "deflate" stream (no zlib header) of known size; false if it
// does not unpack to exactly out_len bytes.
bool inflate_raw(const void* in, size_t in_len, void* out, size_t out_len);

}  // namespace game::setup
