// Controller navigation for the game's touch menus.
//
// The game's UI is a scene graph of im::scene2d_new nodes; everything you can
// tap is an im::scene2d_new::layouts::Button (or a subclass). At start-up we
// find all Button classes through the game's RTTI and hook their render
// method, so each frame we know which buttons are on screen and where. The
// input code then moves a highlight between them with the D-pad/stick and
// "taps" the highlighted one when A is pressed.
#pragma once

#include <string>
#include <vector>

#include "core/common.h"

namespace game::ui {

struct Button {
    u32 node = 0;     // guest pointer to the Button object
    u32 vtable = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // screen rectangle (pixels)
    float cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;  // visible part after clipping
    u32 root = 0;     // top-most ancestor node
    u32 layer = 0;    // index of the scene layer it was drawn in (draw order)
    int order = 0;    // draw order within the frame
    u32 alpha = 255;  // accumulated opacity it was drawn with
    u32 scroll = 0;   // nearest scrolling ancestor (list, carousel), 0 if none
    float clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;  // its visible area
    const char* cls = "";
};

// Find the game's Button classes and hook them. Call once after the game's
// libraries are loaded and relocated, before the game starts.
bool init();
bool active();
// True while the player is driving (keys are driving controls then).
bool is_racing();
// The screen that currently receives input (a popup when one is open), its
// layout root, and whether a screen transition is running.
u32 front_screen();
u32 front_root();
const char* front_screen_id();
bool in_transition();
// The front screen has the game's own controller navigation.
bool front_is_native();

// Call right before each game tick; the buttons drawn during the previous
// tick become visible() and recording starts again.
void frame_begin();
const std::vector<Button>& visible();

// Ask the game whether a button would accept a tap right now (enabled,
// opaque, not locked).
bool is_interactive(const Button& b);

// Mangled class name of a guest object (for logs).
std::string class_name(u32 obj);

void dump(const char* why);

// ---- focus navigation --------------------------------------------------------
enum class Dir { Up, Down, Left, Right };

// Move the highlight in a direction (picks a starting button if none).
enum class Move { None, Moved, Scroll };
// Scroll: the highlighted button is in a list that continues (hidden) in that
// direction; drag the list by (dx, dy) starting at (x, y), then move again.
struct ScrollRequest {
    float x = 0, y = 0, dx = 0, dy = 0;
};
Move move_focus(Dir d, ScrollRequest* scroll = nullptr);
// Centre of the highlighted button (screen pixels), if any.
bool focused_rect_center(float& x, float& y);
// Where a right-stick drag should start: the highlighted button, else the
// middle of the list/map on top. False: nothing suitable (use the centre).
bool drag_origin(float& x, float& y);

u32 focused_node();  // 0 if none
// Layout name of a button ("BTN_ACCELEROMETER", "btn_continue"...).
std::string button_name(u32 node);
// A visible, usable button with this layout name; false if none is on screen.
bool find_button(const char* name, Button& out);
// The highlighted button's centre in screen pixels. Returns false (and picks a
// starting button, so the next press activates it) when nothing was focused.
bool focused_point(float& x, float& y);
bool has_focus();
// Buttons that could be highlighted right now.
size_t candidate_count();
void clear_focus();
// Show/hide the highlight (hidden while the touch screen is being used).
void set_highlight_visible(bool on);
bool highlight_visible();
// A pressed the highlighted button: hide the highlight; if the next screen
// has a button in the same place (continue/OK), highlight that one.
void pressed_focused();
// Once per frame (main thread, outside the game's tick).
void update();
// Draw the highlight over the finished frame.
void draw(int screen_w, int screen_h);

// Set while the input code delivers a controller key to the game (diagnostics).
extern bool g_delivering_key;

}  // namespace game::ui
