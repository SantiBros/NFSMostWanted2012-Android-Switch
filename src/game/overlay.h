// Loader overlay drawn over the game's frame (see overlay.cpp): the
// controller highlight and short text notices.
#pragma once

#include <string>

namespace game::overlay {

struct Color {
    float r, g, b, a;
};
enum class Align { Left, Center, Right };

// The game's own accent red (bottom menu bar, selected tabs).
constexpr Color kGameRed{0.831f, 0.059f, 0.078f, 1.f};

// Look of the controller selection, animated by the caller:
//  pulse 0..1   slow breathing of the glow and the corner brackets
//  flash 0..1   1 right after A is pressed, fading to 0
//  alpha 0..1   fade-in when the selection appears
//  sheen 0..1   position of the light sweeping across the button, < 0 = none
struct SelectionLook {
    float pulse = 0, flash = 0, alpha = 1, sheen = -1;
};
// Records a selection around a screen rectangle (pixels, origin top-left)
// into the current list (between begin() and end()), in the style of the
// game's own selection markers: a soft red glow, a thin red frame just
// outside the button, bright red corner brackets and, now and then, a slanted
// light sweeping across it. Nothing covers the button itself (many of the
// game's buttons are slanted, so a filled box would show its square corners),
// except a short white flash when A is pressed. unit = screen height / 720.
void selection(float x0, float y0, float x1, float y1, float unit, const SelectionLook& look);
// begin() + selection() + end().
void draw_highlight(int screen_w, int screen_h, float x0, float y0, float x1, float y1, const SelectionLook& look);

// Free drawing between begin() and end(): the calls are recorded and end()
// queues them for the GL thread (GL state is saved and restored there).
bool begin(int screen_w, int screen_h);
void end();
void rect(float x0, float y0, float x1, float y1, Color c);
void frame(float x0, float y0, float x1, float y1, float thickness, Color c);
// Gradient quad: colours at the corners top-left, top-right, bottom-right, bottom-left.
void quad(float x0, float y0, float x1, float y1, Color tl, Color tr, Color br, Color bl);
// Any convex quadrilateral: 4 points (x, y) in order, one colour each.
void quad4(const float* xy, const Color* c);
// Soft glow of width w around a rectangle: c at its edge, fading out.
void glow(float x0, float y0, float x1, float y1, float w, Color c);
// Clip what follows to a rectangle, until clip_off().
void clip(float x0, float y0, float x1, float y1);
void clip_off();
// Text in the game's font; y is the baseline, px the font size in pixels.
void text(float x, float y, float px, const std::string& s, Color c, Align align = Align::Left);
float text_width(const std::string& s, float px);

}  // namespace game::overlay
