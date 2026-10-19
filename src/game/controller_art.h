// The picture of the controller on the controls pages (0.3.0).
//
// The game's controls pages (pause > CONTROLS, and Options > Controls) show
// hud/MogaController.tif, a drawing of the MOGA gamepad the game was made for,
// with callout lines that point at its buttons. The picture is in the texture
// atlases of the in-game and menu texture packs
// (published.{1x,2x,4x}/texturepacks_ui/{in_game,menu}.sba, plain RGBA; the
// menu packs' alpha is premultiplied). When the game uploads one of those
// atlases, the port puts its own drawing in the picture's place:
//
//   handheld mode   a handheld console
//   docked          a gamepad
//
// Both are original drawings in the game's style (tools/make_controller_art.py,
// stored in controller_art_images.inc), with the Switch's button names in the
// Switch's arrangement, each element where one of the page's callout lines
// points (game/obb_patch.h moves the one line and swaps the texts that need
// it). When the console is docked or undocked, the port uploads the other
// drawing into the atlases already loaded.
//
// Each atlas is recognised by its size and a checksum of parts of the
// original picture, so nothing else is ever touched; the check costs a few
// microseconds, once per texture, while the game loads.
#pragma once

#include "core/common.h"

namespace game::controller_art {

// Decode the two drawings on a background thread (call early; the first
// atlas the game uploads would otherwise wait for them on its thread).
void prepare();
// For each RGBA (8 bits per channel) texture the game uploads, level 0, with
// the GL name of the texture it goes into.
void patch_texture(u8* rgba, int width, int height, u32 texture);
// The game deleted these textures.
void forget_texture(u32 texture);
// Handheld (true) or docked: which drawing to show. Main thread, outside the
// game's frame (it may upload the other drawing).
void set_handheld(bool handheld);

}  // namespace game::controller_art
