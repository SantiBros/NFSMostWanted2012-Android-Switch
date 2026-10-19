// The picture of the controller on the controls pages (0.3.0).
//
// The game's controls page (pause > CONTROLS, and Options > Controls) shows
// hud/MogaController.tif, a drawing of the MOGA Pro gamepad the game was made
// for, with the MOGA logo in the middle and SELECT / START over its two small
// buttons. The texts around it come from the string tables (game/obb_patch.h);
// the picture itself is in the texture atlases of the in-game and menu
// texture packs (published.{1x,2x,4x}/texturepacks_ui/{in_game,menu}.sba),
// stored as plain RGBA. When the game uploads one of those atlases, the port
// changes the picture in place: the logo is removed (the panel behind it is
// filled in), and SELECT / START become the Switch's - and + . The rest of the
// drawing stays as it is (its L / R and A / B / X / Y labels match the
// Switch's button names).
//
// Each atlas is recognised by its size and a checksum of the parts that
// change, so nothing else is ever touched; the check costs a few microseconds,
// once per texture, while the game loads.
#pragma once

#include "core/common.h"

namespace game::controller_art {

// For each RGBA (8 bits per channel) texture the game uploads, level 0.
void patch_texture(u8* rgba, int width, int height);

}  // namespace game::controller_art
