// Changes to the game's data, made as the game reads it (0.3.0).
//
// The game reads its texts from string tables in the OBB
// (published/stringdata/<language>/nfsmw_android.sb, EA's chunked SBIN
// format). Written for a MOGA controller, they say "MOGA" where the port has
// the Switch's own buttons. The OBB stays as it is on the card: the port
// builds new copies of those entries in memory and the OBB cache shows them
// to the game appended to the file, with a new zip directory that points at
// them (hle/file_cache.h, set_tail). Nothing is written to the card.
//
// The texts changed, in all 11 languages: "MOGA" becomes "Switch" ("SWITCH" in
// capitalised titles), and the Start / Select buttons become + and -; each
// key gets its own new text, since the tables share texts between keys. On
// the menu controls page the texts of the face buttons' callouts trade places,
// and in the layouts (published/layouts/layouts.sb) the two controller pages'
// callouts move, to match the Switch's button arrangement in the port's
// controller pictures (game/controller_art.h).
#pragma once

#include <string>

namespace game::obb_patch {

// Build the changed entries in the background and hand them to the OBB cache.
// Call before the game opens the OBB.
void start(const std::string& obb_host_path);

// One string table with the texts changed; returns the input unchanged if it
// is not one or nothing applies. `changed` counts the texts changed.
std::string patch_string_table(const std::string& sbin, int* changed = nullptr);
// The layouts file with the controller pages' callouts moved; unchanged if it
// is not the 1.3.128 file. `changed` is 1 when they moved.
std::string patch_layouts(const std::string& sbin, int* changed = nullptr);

}  // namespace game::obb_patch
