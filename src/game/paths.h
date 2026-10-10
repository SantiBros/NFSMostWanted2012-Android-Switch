// Where the port keeps its files: everything lives in one folder, the one the
// .nro was started from on the Switch (sd:/switch/NFSMostWanted2012/ by default;
// the folder and the .nro can be renamed), NFSMW_ROOT (or ./gamedata) on the
// desktop.
//
//   NFSMostWanted2012-<version>.nro   the port (any name works)
//   settings.ini        the port's own settings (also changed in the debug menu)
//   log.txt             what happened at the last start (send it with bug reports)
//   game/               files from the player's own copy of the game
//     main.1003128.com.ea.games.nfs13_row.obb   the game data, unchanged
//     lib/              the 5 armeabi-v7a libraries from the APK
//     assets/           the APK's assets/ folder
//   save/               what the game writes
//     progress/         the career save (nfstr_save.sb): money, reputation,
//                       cars and upgrades, events, Most Wanted progress
//     settings/         the game's own settings and small state files
//                       (language, version, agreement accepted, ...)
//
// The game sees Android paths; from_android() maps them into this layout:
//   /data/data/<pkg>/files/var/nfstr_save*      -> save/progress/
//   /data/data/<pkg>/{files,files/var,files/var1}/x, other app files,
//   /sdcard/Android/data/<pkg>/files/x          -> save/settings/x
//   /sdcard/Android/obb/<pkg>/                  -> game/
//   /proc, /sys (CPU and memory information)    -> built into the .nro (romfs)
//
// Folders of earlier versions (lib/, apk/assets/, fs/, the OBB next to the
// .nro, save/internal, save/external, save/system) are moved into this layout
// at start-up, so existing progress is kept. So are the files of the port's
// folder under its earlier names (sd:/switch/NFSMostWanted/, sd:/switch/nfsmw/),
// when it holds this port's files; the old version's .nro is then removed.
#pragma once

#include <string>

namespace game::paths {

// Find the port's folder (Switch: from argv[0], the path of the .nro).
void init(int argc, char** argv);
const std::string& root();
// The .nro that is running (full path), empty when not known.
std::string self_nro();

std::string game_dir();      // <root>/game
std::string lib_dir();       // <root>/game/lib
std::string assets_dir();    // <root>/game/assets
std::string obb();           // the OBB file (in game/, or where an older layout had it)
std::string save_dir();      // <root>/save
std::string progress_dir();  // <root>/save/progress
std::string settings_dir();  // <root>/save/settings
std::string settings_file(); // <root>/settings.ini (the port's settings)
std::string log_file();
// The same, as shown to the player ("switch/NFSMostWanted2012/log.txt").
std::string display(const std::string& host_path);

// Host path for an absolute, normalized Android path the game uses.
std::string from_android(const std::string& path);

// Move the files of earlier layouts into this one and create the folders.
// Call once at start-up, after init().
void prepare();

}  // namespace game::paths
