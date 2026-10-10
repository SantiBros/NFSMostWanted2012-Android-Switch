// Switch controls -> what the game understands.
//
// NFS:MW for Android supports MOGA gamepads (com.bda.controller). We present a
// connected "MOGA Pro" and feed it from the platform's controller state: the
// game gets both key events and analog axes (steering/throttle), exactly like
// on a phone with a MOGA clipped on. The touch screen is forwarded as touches.
#include "game/input.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <vector>
#include <mutex>
#include <string>

#include "core/memory.h"
#include "game/config.h"
#include "game/debug_menu.h"
#include "game/ui_nav.h"
#include "game/overlay.h"
#include "game/motion.h"
#include "hle/jni/jni.h"
#include "platform/platform.h"

namespace jni {
jref activity();
jref surface_view();
}

namespace game::input {

namespace {
using jni::Call;
using jni::jref;
using V = jni::Value;

// Android / MOGA key codes
enum : int {
    KEY_DPAD_UP = 19, KEY_DPAD_DOWN = 20, KEY_DPAD_LEFT = 21, KEY_DPAD_RIGHT = 22,
    KEY_BUTTON_A = 96, KEY_BUTTON_B = 97, KEY_BUTTON_X = 99, KEY_BUTTON_Y = 100,
    KEY_BUTTON_L1 = 102, KEY_BUTTON_R1 = 103, KEY_BUTTON_L2 = 104, KEY_BUTTON_R2 = 105,
    KEY_BUTTON_START = 108, KEY_BUTTON_SELECT = 109, KEY_BACK = 4,
};
// MOGA axis ids
enum : int { AXIS_X = 0, AXIS_Y = 1, AXIS_Z = 11, AXIS_RZ = 14, AXIS_LTRIGGER = 17, AXIS_RTRIGGER = 18 };

struct Mapping {
    unsigned button;
    int key;
};
// Switch button -> MOGA key. In races the game maps A=drift, L1=brake, R1=nitro.
const Mapping kMap[] = {
    {platform::BTN_A, KEY_BUTTON_A},       {platform::BTN_B, KEY_BUTTON_B},
    {platform::BTN_X, KEY_BUTTON_X},       {platform::BTN_Y, KEY_BUTTON_Y},
    {platform::BTN_L, KEY_BUTTON_L1},      {platform::BTN_R, KEY_BUTTON_R1},
    {platform::BTN_ZL, KEY_BUTTON_L2},     {platform::BTN_ZR, KEY_BUTTON_R2},
    {platform::BTN_PLUS, KEY_BUTTON_START}, {platform::BTN_MINUS, KEY_BUTTON_SELECT},
    {platform::BTN_UP, KEY_DPAD_UP},       {platform::BTN_DOWN, KEY_DPAD_DOWN},
    {platform::BTN_LEFT, KEY_DPAD_LEFT},   {platform::BTN_RIGHT, KEY_DPAD_RIGHT},
};

std::mutex g_mutex;
platform::InputState g_state, g_prev;
jref g_listener = 0;  // the game's MogaController (ControllerListener)
jref g_controller = 0;

float axis(const platform::InputState& s, int a) {
    switch (a) {
    case AXIS_X: return s.lx;
    case AXIS_Y: return s.ly;
    // The game only reacts to the right stick when it is exactly at -1 or +1
    // (nitro up, brake down), as on the MOGA Pro: snap near-full deflection.
    case AXIS_Z: return s.rx > 0.75f ? 1.f : s.rx < -0.75f ? -1.f : 0.f;
    case AXIS_RZ: return s.ry > 0.75f ? 1.f : s.ry < -0.75f ? -1.f : 0.f;
    case AXIS_LTRIGGER: return s.lt;
    case AXIS_RTRIGGER: return s.rt;
    default: return 0;
    }
}
bool key_down(const platform::InputState& s, int key) {
    for (auto& m : kMap)
        if (m.key == key) return (s.buttons & m.button) != 0;
    return false;
}
s64 now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void send_key(int key, bool down) {
    struct Flag {
        Flag() { game::ui::g_delivering_key = true; }
        ~Flag() { game::ui::g_delivering_key = false; }
    } flag;
    if (g_listener) {
        jref ev = jni::new_object("com/bda/controller/KeyEvent");
        jni::set_field(ev, "keyCode", V::I(key));
        jni::set_field(ev, "action", V::I(down ? 0 : 1));
        jni::call_native("com/ea/ironmonkey/MogaController", "nativeOnKeyEvent", "(Lcom/bda/controller/KeyEvent;)V",
                         g_listener, {V::L(ev)});
    } else {
        jni::call_native("com/ea/ironmonkey/GameActivityMain",
                         down ? "nativeOnPhysicalKeyDown" : "nativeOnPhysicalKeyUp", "(II)V", jni::activity(),
                         {V::I(key), V::I(0)});
    }
}

void send_motion() {
    if (!g_listener) return;
    jref ev = jni::new_object("com/bda/controller/MotionEvent");
    jni::call_native("com/ea/ironmonkey/MogaController", "nativeOnMotionEvent", "(Lcom/bda/controller/MotionEvent;)V",
                     g_listener, {V::L(ev)});
}

void send_touch(int action, float x, float y) {
    jni::call_native("com/ea/ironmonkey/GameGLSurfaceView", "nativeTouchScreenEvent", "(IIFF)V", jni::surface_view(),
                     {V::I(action), V::I(0), V::F(x), V::F(y)});
}

}  // namespace

void register_java() {
    using jni::def;
    const char* C = "com/bda/controller/Controller";
    def(C, "getInstance", "(Landroid/content/Context;)Lcom/bda/controller/Controller;", [](Call&) {
        if (!g_controller) g_controller = jni::new_object("com/bda/controller/Controller");
        return V::L(g_controller);
    });
    def(C, "init", "()Z", [](Call&) { return V::Z(true); });
    def(C, "exit", "()V", [](Call&) { return V{}; });
    def(C, "onPause", "()V", [](Call&) { return V{}; });
    def(C, "onResume", "()V", [](Call&) { return V{}; });
    def(C, "setListener", "(Lcom/bda/controller/ControllerListener;Landroid/os/Handler;)V", [](Call& c) {
        g_listener = c.l(0);
        LOGI("MOGA listener registered: controller input enabled");
        return V{};
    });
    def(C, "getState", "(I)I", [](Call& c) {
        switch (c.i(0)) {
        case 1: return V::I(1);  // STATE_CONNECTION -> ACTION_CONNECTED
        case 4: return V::I(1);  // STATE_CURRENT_PRODUCT_VERSION -> MOGA Pro
        case 3: return V::I(1);  // STATE_SUPPORTED_PRODUCT_VERSION
        default: return V::I(0);
        }
    });
    def(C, "getInfo", "(I)I", [](Call& c) { return V::I(c.i(0) == 1 || c.i(0) == 2 ? 1 : 0); });
    def(C, "getKeyCode", "(I)I", [](Call& c) {
        std::lock_guard lk(g_mutex);
        return V::I(key_down(g_state, c.i(0)) ? 0 : 1);  // ACTION_DOWN / ACTION_UP
    });
    def(C, "getAxisValue", "(I)F", [](Call& c) {
        std::lock_guard lk(g_mutex);
        return V::F(axis(g_state, c.i(0)));
    });
    def("android/content/Context", "bindService", "(Landroid/content/Intent;Landroid/content/ServiceConnection;I)Z",
        [](Call&) { return V::Z(true); });
    def("com/ea/ironmonkey/GameActivityMain", "bindService",
        "(Landroid/content/Intent;Landroid/content/ServiceConnection;I)Z", [](Call&) { return V::Z(true); });

    const char* KE = "com/bda/controller/KeyEvent";
    def(KE, "getKeyCode", "()I", [](Call& c) { return jni::get(c.thiz)->fields["keyCode"]; });
    def(KE, "getAction", "()I", [](Call& c) { return jni::get(c.thiz)->fields["action"]; });
    def(KE, "getControllerId", "()I", [](Call&) { return V::I(1); });
    def(KE, "getEventTime", "()J", [](Call&) { return V::J(now_ms()); });

    const char* ME = "com/bda/controller/MotionEvent";
    def(ME, "getAxisValue", "(I)F", [](Call& c) {
        std::lock_guard lk(g_mutex);
        float v = axis(g_state, c.i(0));
        LOGV("MOGA getAxisValue(%d) = %.2f", c.i(0), v);
        return V::F(v);
    });
    def(ME, "getAxisValue", "(II)F", [](Call& c) {
        std::lock_guard lk(g_mutex);
        return V::F(axis(g_state, c.i(0)));
    });
    def(ME, "getX", "()F", [](Call&) { std::lock_guard lk(g_mutex); return V::F(g_state.lx); });
    def(ME, "getY", "()F", [](Call&) { std::lock_guard lk(g_mutex); return V::F(g_state.ly); });
    def(ME, "getX", "(I)F", [](Call&) { std::lock_guard lk(g_mutex); return V::F(g_state.lx); });
    def(ME, "getY", "(I)F", [](Call&) { std::lock_guard lk(g_mutex); return V::F(g_state.ly); });
    def(ME, "getRawX", "(I)F", [](Call&) { std::lock_guard lk(g_mutex); return V::F(g_state.lx); });
    def(ME, "getRawY", "(I)F", [](Call&) { std::lock_guard lk(g_mutex); return V::F(g_state.ly); });
    def(ME, "getPointerCount", "()I", [](Call&) { return V::I(1); });
    def(ME, "getPointerId", "(I)I", [](Call&) { return V::I(0); });
    def(ME, "findPointerIndex", "(I)I", [](Call&) { return V::I(0); });
    def(ME, "getXPrecision", "()F", [](Call&) { return V::F(1.f); });
    def(ME, "getYPrecision", "()F", [](Call&) { return V::F(1.f); });
    def(ME, "getControllerId", "()I", [](Call&) { return V::I(1); });
    def(ME, "getEventTime", "()J", [](Call&) { return V::J(now_ms()); });
}

// ---- menu navigation ----------------------------------------------------------
// Outside races the D-pad / left stick move a highlight between the buttons
// on screen and A taps the highlighted one (game::ui). Those inputs are then
// kept from the game; every other button still reaches it, and the touch
// screen keeps working as before.
namespace {
struct Repeat {
    bool held = false;
    s64 next = 0;
    // True on the press and then every 140 ms after an initial 380 ms.
    bool fire(bool down, s64 now) {
        if (!down) {
            held = false;
            return false;
        }
        if (!held) {
            held = true;
            next = now + 380;
            return true;
        }
        if (now >= next) {
            next = now + 140;
            return true;
        }
        return false;
    }
} g_rep[4];

// Synthetic touch gestures (taps and drags), one step per frame.
struct Gesture {
    struct Step {
        int action;  // 0 down, 1 up, 2 move, -1 wait
        float x, y;
    };
    std::vector<Step> steps;
    size_t next = 0;
    bool busy() const { return next < steps.size(); }
    void run_step() {
        if (!busy()) return;
        const Step& s = steps[next++];
        if (s.action >= 0) send_touch(s.action, s.x, s.y);
    }
} g_gesture;

void start_tap(float x, float y) {
    g_gesture = {};
    g_gesture.steps = {{0, x, y}, {-1, x, y}, {1, x, y}};
}

// A finger drag from (x0,y0) to (x1,y1): scrolls lists, pans the map.
void start_drag(float x0, float y0, float x1, float y1, int frames) {
    g_gesture = {};
    g_gesture.steps.push_back({0, x0, y0});
    for (int i = 1; i <= frames; i++)
        g_gesture.steps.push_back({2, x0 + (x1 - x0) * i / frames, y0 + (y1 - y0) * i / frames});
    g_gesture.steps.push_back({2, x1, y1});  // pause at the end: no fling
    g_gesture.steps.push_back({-1, x1, y1});
    g_gesture.steps.push_back({1, x1, y1});
}

// Right stick in menus: a finger held on the screen and moved by the stick.
struct StickDrag {
    bool active = false;
    float x = 0, y = 0, x0 = 0, y0 = 0;
} g_stick_drag;
bool g_nav_mode = false;  // last frame was menus (not racing)
bool g_nav_disabled = false;  // debugging: pass everything to the game

constexpr unsigned kNavButtons =
    platform::BTN_UP | platform::BTN_DOWN | platform::BTN_LEFT | platform::BTN_RIGHT | platform::BTN_A;

// Right stick in menus: drag the screen like a finger (scroll lists, pan the
// map, turn the car).
void stick_drag(const platform::InputState& s) {
    float mag = std::max(std::fabs(s.rx), std::fabs(s.ry));
    if (mag > 0.25f && !g_gesture.busy()) {
        const auto& S = game::settings();
        float speed = 14.f * S.height / 720.f;
        if (!g_stick_drag.active) {
            float fx, fy;
            if (!game::ui::drag_origin(fx, fy)) fx = S.width / 2.f, fy = S.height / 2.f;
            g_stick_drag = {true, fx, fy, fx, fy};
            send_touch(0, fx, fy);
        } else {
            g_stick_drag.x = std::clamp(g_stick_drag.x + s.rx * speed, 1.f, S.width - 2.f);
            g_stick_drag.y = std::clamp(g_stick_drag.y + s.ry * speed, 1.f, S.height - 2.f);
            send_touch(2, g_stick_drag.x, g_stick_drag.y);
        }
    } else if (g_stick_drag.active) {
        // A short nudge must not "tap" the button under the finger: cancel
        // the touch instead of lifting it (Android ACTION_CANCEL).
        float moved = std::fabs(g_stick_drag.x - g_stick_drag.x0) + std::fabs(g_stick_drag.y - g_stick_drag.y0);
        send_touch(moved < 20.f ? 3 : 1, g_stick_drag.x, g_stick_drag.y);
        g_stick_drag.active = false;
    }
}

// ---- gyro steering on/off -----------------------------------------------------
// The "TILT TO STEER" tile in Options > Controls switches the loader's gyro
// steering on and off (game::motion). With a gamepad connected the game would
// only answer that tile with "please disconnect your MOGA controller".
constexpr const char* kTiltTile = "BTN_ACCELEROMETER";

struct Notice {
    std::string title, detail;
    s64 until = 0;
} g_notice;

void show_notice(const std::string& title, const std::string& detail, int ms) {
    g_notice = {title, detail, now_ms() + ms};
}

void toggle_gyro() {
    bool on = !game::motion::enabled();
    game::motion::set_enabled(on);
    if (on)
        show_notice("GYRO STEERING ON",
                    game::motion::sensor_seen() ? "Tilt the console or controller to steer. The stick and buttons still work."
                                                : "No motion sensor found on this controller yet.",
                    4000);
    else
        show_notice("GYRO STEERING OFF", "Steer with the left stick.", 3000);
}

bool inside(const game::ui::Button& b, float x, float y) {
    return x >= b.cx0 && x < b.cx1 && y >= b.cy0 && y < b.cy1;
}

// Returns the buttons (of kNavButtons) that should still reach the game.
unsigned navigate(const platform::InputState& s, const platform::InputState& prev) {
    s64 now = now_ms();
    const float t = 0.6f;
    bool dir[4] = {
        (s.buttons & platform::BTN_UP) || s.ly < -t,
        (s.buttons & platform::BTN_DOWN) || s.ly > t,
        (s.buttons & platform::BTN_LEFT) || s.lx < -t,
        (s.buttons & platform::BTN_RIGHT) || s.lx > t,
    };
    const game::ui::Dir dirs[4] = {game::ui::Dir::Up, game::ui::Dir::Down, game::ui::Dir::Left, game::ui::Dir::Right};
    static int after_scroll = -1;  // direction to move once a list drag is done
    if (after_scroll >= 0 && !g_gesture.busy()) {
        game::ui::move_focus(dirs[after_scroll]);
        after_scroll = -1;
    }
    for (int i = 0; i < 4; i++)
        if (g_rep[i].fire(dir[i], now) && !g_gesture.busy()) {
            game::ui::ScrollRequest r;
            if (game::ui::move_focus(dirs[i], &r) == game::ui::Move::Scroll) {
                // The list goes on past its edge: drag it like a finger would,
                // then highlight the button that came into view.
                start_drag(r.x, r.y, r.x + r.dx, r.y + r.dy, 8);
                after_scroll = i;
            }
            game::ui::set_highlight_visible(true);
        }
    // L/R move the game's own selection in the bottom menu bar: the next A
    // then confirms that selection (the game's "L R MENU" / "A CONFIRM").
    static bool menu_armed = false;
    unsigned lr = platform::BTN_L | platform::BTN_R;
    if ((s.buttons & lr) & ~(prev.buttons & lr)) {
        game::ui::clear_focus();
        menu_armed = true;
    }
    for (int i = 0; i < 4; i++)
        if (dir[i]) menu_armed = false;

    static bool a_to_game = false;  // this A press belongs to the game
    bool a = s.buttons & platform::BTN_A, a_prev = prev.buttons & platform::BTN_A;
    if (a && !a_prev && !g_gesture.busy()) {
        float x, y;
        if (game::ui::highlight_visible() && game::ui::focused_point(x, y) &&
            game::ui::button_name(game::ui::focused_node()) == kTiltTile) {
            toggle_gyro();  // instead of the game's "disconnect your MOGA" message
            a_to_game = false;
        } else if (game::ui::highlight_visible() && game::ui::focused_point(x, y)) {
            // Tap the highlighted button. The highlight hides until the next
            // D-pad press, since the screen usually changes now.
            game::ui::pressed_focused();
            start_tap(x, y);
            a_to_game = false;
        } else if (menu_armed) {
            a_to_game = true;  // confirm the menu bar selection made with L/R
            menu_armed = false;
        } else if (game::ui::candidate_count() == 0) {
            // Nothing to highlight ("tap to continue" screens): tap the centre.
            int w = game::settings().width, h = game::settings().height;
            start_tap(w / 2.f, h / 2.f);
            a_to_game = false;
        } else {
            // Nothing highlighted yet: show the highlight; the next A presses it.
            // (A does not go to the game here: on some screens the game's own
            // A handling does something else, e.g. "quit" in the pause menu.)
            if (!game::ui::has_focus()) game::ui::focused_point(x, y);  // picks a starting button
            game::ui::set_highlight_visible(true);
            a_to_game = false;
        }
    }
    if (!a && !a_prev) a_to_game = false;

    return a_to_game ? platform::BTN_A : 0u;
}
}  // namespace

void set_navigation_enabled(bool on) { g_nav_disabled = !on; }

void draw_overlay(int screen_w, int screen_h) {
    using namespace game::overlay;
    game::ui::Button tile;
    bool tile_on_screen = game::ui::find_button(kTiltTile, tile);
    s64 now = now_ms();
    bool notice = now < g_notice.until;
    if (!tile_on_screen && !notice) return;
    if (!begin(screen_w, screen_h)) return;
    const float u = screen_h / 720.f;
    if (tile_on_screen) {
        // State badge across the top of the tile.
        bool on = game::motion::enabled();
        float x0 = tile.cx0 + 10 * u, x1 = tile.cx1 - 10 * u, y0 = tile.cy0 + 10 * u, y1 = y0 + 34 * u;
        if (on) rect(x0, y0, x1, y1, {kGameRed.r, kGameRed.g, kGameRed.b, 0.92f});
        else rect(x0, y0, x1, y1, {0.f, 0.f, 0.f, 0.72f});
        text((x0 + x1) / 2, y0 + 24 * u, 19 * u, on ? "GYRO: ON" : "GYRO: OFF",
             on ? Color{1.f, 1.f, 1.f, 1.f} : Color{0.75f, 0.75f, 0.75f, 1.f}, Align::Center);
    }
    if (notice) {
        float fade = std::min(1.f, (g_notice.until - now) / 400.f);
        float w1 = text_width(g_notice.title, 30 * u), w2 = text_width(g_notice.detail, 19 * u);
        float w = std::max(w1, w2) + 48 * u;
        float cx = screen_w / 2.f, y0 = 70 * u, y1 = y0 + 86 * u;
        rect(cx - w / 2, y0, cx + w / 2, y1, {0.f, 0.f, 0.f, 0.8f * fade});
        rect(cx - w / 2, y0, cx + w / 2, y0 + 4 * u, {kGameRed.r, kGameRed.g, kGameRed.b, fade});
        text(cx, y0 + 42 * u, 30 * u, g_notice.title, {1.f, 1.f, 1.f, fade}, Align::Center);
        text(cx, y0 + 72 * u, 19 * u, g_notice.detail, {0.85f, 0.85f, 0.85f, fade}, Align::Center);
    }
    end();
}

namespace {
// Hand the state to the game: MOGA key events for changed buttons, a motion
// event for moved axes, and touch events.
void deliver(const platform::InputState& s) {
    {
        std::lock_guard lk(g_mutex);
        g_prev = g_state;
        g_state = s;
    }
    for (auto& m : kMap) {
        bool now = s.buttons & m.button, before = g_prev.buttons & m.button;
        if (now != before) send_key(m.key, now);
    }
    if (s.lx != g_prev.lx || s.ly != g_prev.ly || s.rx != g_prev.rx || s.ry != g_prev.ry || s.lt != g_prev.lt ||
        s.rt != g_prev.rt)
        send_motion();
    if (s.touch && !g_prev.touch) send_touch(0, s.touch_x, s.touch_y);
    else if (s.touch && (s.touch_x != g_prev.touch_x || s.touch_y != g_prev.touch_y)) send_touch(2, s.touch_x, s.touch_y);
    else if (!s.touch && g_prev.touch) send_touch(1, g_prev.touch_x, g_prev.touch_y);
}
}  // namespace

void pump() {
    platform::InputState raw;
    if (!platform::poll_input(raw)) return;
    static platform::InputState raw_prev;

    g_gesture.run_step();  // synthetic taps/drags, one step per frame

    // Debug menu (ZL + ZR + Minus). While it is open the game gets no input;
    // opening it in a race first pauses the race with the game's own pause
    // (+ / START). Buttons still held when it closes are kept from the game
    // until they are released.
    static unsigned swallow = 0;
    static int pause_frames = 0;
    static bool combo_armed = true;  // after the menu closes: once ZL, ZR and Minus are all released
    const unsigned combo_buttons = platform::BTN_ZL | platform::BTN_ZR | platform::BTN_MINUS;
    if (!(raw.buttons & combo_buttons)) combo_armed = true;
    const bool was_open = debug_menu::is_open();
    if (combo_armed && debug_menu::combo_pressed(raw, raw_prev)) {
        if (was_open) {
            debug_menu::close();
        } else {
            debug_menu::open();
            pause_frames = game::ui::active() && game::ui::is_racing() ? 2 : 0;
        }
    } else if (was_open) {
        debug_menu::update(raw, raw_prev);
    }
    if (was_open || debug_menu::is_open()) {
        if (!debug_menu::is_open()) {  // just closed
            swallow = raw.buttons;
            combo_armed = false;
        }
        platform::InputState s{};
        if (pause_frames > 0) {
            if (pause_frames == 2) s.buttons = platform::BTN_PLUS;  // press, then release next frame
            pause_frames--;
        }
        if (g_stick_drag.active) {
            send_touch(3, g_stick_drag.x, g_stick_drag.y);
            g_stick_drag.active = false;
        }
        raw_prev = raw;
        deliver(s);
        return;
    }
    swallow &= raw.buttons;  // released buttons reach the game again

    bool menus = game::ui::active() && !game::ui::is_racing() && !g_nav_disabled;
    if (menus != g_nav_mode) {
        g_nav_mode = menus;
        if (!menus) game::ui::clear_focus();
    }
    platform::InputState s = raw;  // what the game gets to see
    s.buttons &= ~swallow;

    // A touch on the TILT TO STEER tile toggles gyro steering (on release
    // inside the tile) and is kept from the game.
    static bool tilt_touch = false;
    if (raw.touch && !raw_prev.touch) {
        game::ui::Button tile;
        tilt_touch = menus && game::ui::find_button(kTiltTile, tile) && inside(tile, raw.touch_x, raw.touch_y);
    }
    if (tilt_touch) {
        s.touch = false;
        if (!raw.touch) {
            game::ui::Button tile;
            if (game::ui::find_button(kTiltTile, tile) && inside(tile, raw_prev.touch_x, raw_prev.touch_y)) toggle_gyro();
            tilt_touch = false;
        }
    }

    // Gyro steering while driving: the tilt becomes the left stick's steering
    // value whenever the stick itself is left alone.
    bool racing = game::ui::active() && game::ui::is_racing();
    float tilt = 0;
    bool have_tilt = game::motion::steering(raw, tilt);
    static bool was_racing = false;
    if (racing && !was_racing) {
        game::motion::reset();
        if (game::motion::enabled()) show_notice("GYRO STEERING ON", "Hold the console level to drive straight.", 2500);
    }
    was_racing = racing;
    if (racing && have_tilt && std::fabs(raw.lx) < 0.15f) s.lx = std::round(tilt * 200.f) / 200.f;

    if (menus && game::ui::front_is_native()) {
        game::ui::clear_focus();  // the game shows its own highlight here
        stick_drag(raw);
        s.rx = s.ry = 0;
    } else if (menus) {
        unsigned pass = navigate(raw, raw_prev);
        stick_drag(raw);
        s.buttons &= ~(kNavButtons & ~pass);
        s.lx = s.ly = 0;
        s.rx = s.ry = 0;
    } else if (g_stick_drag.active) {
        send_touch(3, g_stick_drag.x, g_stick_drag.y);  // a race started mid-drag
        g_stick_drag.active = false;
    }
    if (raw.touch) game::ui::set_highlight_visible(false);  // touch screen in use
    raw_prev = raw;
    deliver(s);
}

}  // namespace game::input
