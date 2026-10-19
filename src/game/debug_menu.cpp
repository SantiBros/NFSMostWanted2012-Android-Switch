#include "game/debug_menu.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "game/config.h"
#include "game/overlay.h"
#include "game/perf.h"
#include "game/prefs.h"
#include "hle/hle.h"

namespace game::debug_menu {

namespace {
using platform::InputState;
using prefs::Values;

s64 now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct Choice {
    int value;
    std::string text;
    std::string note;  // shown in orange when this value is chosen (side effects)
};
enum class Kind { Setting, Restore, Close };
struct Item {
    Kind kind;
    std::string label;
    int Values::*field_int = nullptr;
    bool Values::*field_bool = nullptr;
    std::vector<Choice> choices;
    bool restart = false;      // takes effect at the next start
    bool switch_only = false;  // console clocks
    std::string help;
};

bool g_open = false;
int g_sel = 0;
std::vector<Item> g_items;
std::string g_flash;  // short confirmation under the help text
s64 g_flash_until = 0;

struct Repeat {
    bool held = false;
    s64 next = 0;
    bool fire(bool down, s64 now) {
        if (!down) return held = false;
        if (!held) {
            held = true;
            next = now + 350;
            return true;
        }
        if (now >= next) {
            next = now + 110;
            return true;
        }
        return false;
    }
} g_rep[4];

// Layout of the last drawn frame (for the touch screen).
struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool in(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};
Rect g_panel;
std::vector<Rect> g_rows;
float g_value_x0 = 0, g_value_mid = 0;
bool g_touching = false;
float g_touch_x = 0, g_touch_y = 0;

std::string mhz_text(int khz) {
    char b[32];
    if (khz % 1000) std::snprintf(b, sizeof b, "%.1f MHz", khz / 1000.0);
    else std::snprintf(b, sizeof b, "%d MHz", khz / 1000);
    return b;
}

void build_items() {
    const bool docked = platform::is_docked();
    g_items.clear();

    Item fps{Kind::Setting, "Frame rate limit"};
    fps.field_int = &Values::fps_limit;
    fps.choices = {{30, "30 FPS", ""},
                   {60, "60 FPS", "Only smoother when the game really reaches 60; otherwise motion gets uneven."},
                   {0, "Off", "Frames come as fast as they can, so motion is uneven."}};
    fps.help = "The game is made for 30 FPS. Below 30 it does not skip ahead but slows down, so a steady 30 "
               "matters most.";
    g_items.push_back(fps);

    Item vs{Kind::Setting, "VSync"};
    vs.field_bool = &Values::vsync;
    vs.choices = {{1, "On", ""}, {0, "Off", "The picture can tear (a visible line where two frames meet)."}};
    vs.help = "Shows each frame at the screen's refresh. Off can lower input delay a little, but tears.";
    g_items.push_back(vs);

    Item res{Kind::Setting, "3D resolution"};
    res.field_int = &Values::scene_scale;
    int ow, oh;
    perf::output_size(ow, oh);  // the real screen (it changes when docking / undocking)
    for (int pct : {100, 90, 80, 75, 67, 50}) {
        char t[48];
        std::snprintf(t, sizeof t, "%d%%  (%dx%d)", pct, (int)std::lround(ow * pct / 100.0),
                      (int)std::lround(oh * pct / 100.0));
        res.choices.push_back({pct, t, pct <= 67 ? "Noticeably blurrier 3D scene." : ""});
    }
    res.help = "Resolution of the race scene. Lower = less work for the GPU; menus and the HUD stay sharp. "
               "Works at detail level Highest, the level that draws the scene off-screen for its speed blur.";
    g_items.push_back(res);

    Item cpu{Kind::Setting, "CPU clock"};
    cpu.field_int = &Values::cpu_khz;
    cpu.switch_only = true;
    const char* above = "Above the console's normal clock: more heat and battery use.";
    cpu.choices = {{prefs::kClockStock, "Stock (1020 MHz)", ""},
                   {1224000, "1224 MHz", "A clock some Switch games use; slightly more battery use."},
                   {1428000, "1428 MHz", above},
                   {1581000, "1581 MHz", above},
                   {1785000, "1785 MHz", above}};
    cpu.help = "The translated game code runs on the CPU. The port is set up for the stock clock; raise it "
               "only if the frame-rate counter stays under 30 FPS (below 30 the game slows down).";
    g_items.push_back(cpu);

    Item gpu{Kind::Setting, "GPU clock"};
    gpu.field_int = &Values::gpu_khz;
    gpu.switch_only = true;
    const char* hot = docked ? "" : "Above Nintendo's handheld limit: more heat and battery drain.";
    const char* above_hh = docked ? "" : "Above the normal handheld clock: more battery use.";
    const char* low = docked ? "Below the normal docked clock: slower graphics." : "";
    gpu.choices = {{prefs::kClockAuto, docked ? "Auto  (stock when docked)" : "Auto  (460.8 MHz handheld)", ""},
                   {prefs::kClockStock, docked ? "Stock (768 MHz)" : "Stock (307.2 MHz)", ""},
                   {307200, "307.2 MHz", low},
                   {384000, "384 MHz", docked ? low : above_hh},
                   {460800, "460.8 MHz", docked ? low : above_hh},
                   {614400, "614.4 MHz", docked ? low : hot},
                   {768000, "768 MHz", hot}};
    gpu.help = "The game is light on the GPU. Auto uses 460.8 MHz in handheld mode (the highest handheld "
               "clock Nintendo uses) and the normal clock when docked.";
    g_items.push_back(gpu);

    Item mem{Kind::Setting, "Memory clock"};
    mem.field_int = &Values::mem_khz;
    mem.switch_only = true;
    mem.choices = {{prefs::kClockStock, docked ? "Stock (1600 MHz)" : "Stock (1331.2 MHz)", ""},
                   {1331200, "1331.2 MHz", "Slower memory: a lower frame rate in busy scenes."},
                   {1600000, "1600 MHz", docked ? "" : "Above the normal handheld clock: more battery use."}};
    mem.help = "1600 MHz is the docked memory clock; Nintendo also uses it in handheld mode.";
    g_items.push_back(mem);

    Item q{Kind::Setting, "Detail level"};
    q.field_int = &Values::quality;
    q.restart = true;
    q.choices = {{prefs::kQualityHighest, "Highest", "Adds a speed-blur pass; needs the most CPU time and GL calls."},
                 {prefs::kQualityHigh, "High", "Like Highest without the speed blur; about as much CPU time."},
                 {prefs::kQualityMedium, "Medium", ""},
                 {prefs::kQualityLow, "Low", ""}};
    q.help = std::string("The game's own detail level. Medium and Low draw about a quarter fewer objects per "
                         "frame, which saves CPU time. Running now: ") +
             perf::tier_name(perf::startup_tier()) + ".";
    g_items.push_back(q);

    Item gt{Kind::Setting, "GL on its own core"};
    gt.field_bool = &Values::gl_thread;
    gt.restart = true;
    gt.choices = {{1, "On", ""},
                  {0, "Off", "The game thread then also does the graphics driver's work: a lower frame rate."}};
    gt.help = "Runs the game's graphics calls on a separate CPU core, so the game code does not wait for the "
              "driver or for the screen refresh.";
    g_items.push_back(gt);

    Item dm{Kind::Setting, "Direct memory access"};
    dm.field_bool = &Values::direct_memory;
    dm.restart = true;
    dm.switch_only = true;
    dm.choices = {{1, "On", ""},
                  {0, "Off", "Every memory access of the game code goes through a lookup table: slower."}};
    dm.help = "The translated game code reads and writes its memory directly instead of through a lookup "
              "table. Turn it off only if the game crashes.";
    g_items.push_back(dm);

    Item snd{Kind::Setting, "Sound volume"};
    snd.field_int = &Values::sound_volume;
    for (int pct : {100, 90, 80, 70, 60, 50})
        snd.choices.push_back({pct, std::to_string(pct) + "%",
                               pct > 70 ? "Loud moments are cut off and crackle (more the higher)." : ""});
    snd.help = "Volume of the game's sound mix. The game mixes so loud that loud moments (engines, crashes, "
               "music) are cut off and crackle; a lower value leaves room for them. Turn the console's volume up "
               "instead.";
    g_items.push_back(snd);

    Item ov{Kind::Setting, "Frame-rate counter"};
    ov.field_int = &Values::overlay;
    ov.choices = {{prefs::kOverlayOff, "Off", ""}, {prefs::kOverlayFps, "FPS", ""}, {prefs::kOverlayFull, "Full", ""}};
    ov.help = "Shows the frame rate in the top-left corner. Full adds frame times, clocks and resolution.";
    g_items.push_back(ov);

    Item rs{Kind::Restore, "Restore recommended settings"};
    rs.help = "30 FPS with VSync, full 3D resolution, stock clocks, detail level Medium, GL on its own core, "
              "direct memory access, sound volume 70%, counter off.";
    g_items.push_back(rs);

    Item cl{Kind::Close, "Close"};
    cl.help = "Settings are saved as you change them (settings.ini, next to the .nro).";
    g_items.push_back(cl);
}

int get_value(const Item& it) {
    auto& v = prefs::get();
    if (it.field_int) return v.*it.field_int;
    if (it.field_bool) return (v.*it.field_bool) ? 1 : 0;
    return 0;
}

int current_index(const Item& it) {
    int v = get_value(it);
    for (size_t i = 0; i < it.choices.size(); i++)
        if (it.choices[i].value == v) return (int)i;
    return -1;  // a value typed into settings.ini by hand
}

void changed(const Item& it) {
    prefs::save();
    if (!it.restart) perf::apply();
}

void set_index(Item& it, int idx) {
    if (it.choices.empty()) return;
    idx = std::clamp(idx, 0, (int)it.choices.size() - 1);
    int v = it.choices[idx].value;
    auto& vals = prefs::get();
    if (it.field_int) vals.*it.field_int = v;
    if (it.field_bool) vals.*it.field_bool = v != 0;
    LOGI("debug menu: %s = %s", it.label.c_str(), it.choices[idx].text.c_str());
    changed(it);
}

void step(Item& it, int dir, bool wrap) {
    if (it.kind != Kind::Setting || it.choices.empty()) return;
    int n = (int)it.choices.size();
    int i = current_index(it);
    if (i < 0) i = dir > 0 ? -1 : n;
    int j = i + dir;
    if (wrap) j = (j % n + n) % n;
    else j = std::clamp(j, 0, n - 1);
    if (j != i) set_index(it, j);
}

void flash(const std::string& s) {
    g_flash = s;
    g_flash_until = now_ms() + 2500;
}

void activate(Item& it) {
    switch (it.kind) {
    case Kind::Setting: step(it, +1, true); break;
    case Kind::Restore: {
        auto& v = prefs::get();
        Values d = prefs::defaults();
        // Performance and sound; gyro steering stays as it is.
        v.fps_limit = d.fps_limit;
        v.vsync = d.vsync;
        v.scene_scale = d.scene_scale;
        v.quality = d.quality;
        v.cpu_khz = d.cpu_khz;
        v.gpu_khz = d.gpu_khz;
        v.mem_khz = d.mem_khz;
        v.overlay = d.overlay;
        v.sound_volume = d.sound_volume;
        v.gl_thread = d.gl_thread;
        v.direct_memory = d.direct_memory;
        prefs::save();
        perf::apply();
        LOGI("debug menu: recommended settings restored");
        flash("Recommended settings restored.");
        break;
    }
    case Kind::Close: close(); break;
    }
}

// Word-wrap for the help text.
std::vector<std::string> wrap(const std::string& s, float px, float width) {
    std::vector<std::string> lines;
    std::string cur;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        std::string word = s.substr(i, j - i);
        std::string trial = cur.empty() ? word : cur + " " + word;
        if (!cur.empty() && overlay::text_width(trial, px) > width) {
            lines.push_back(cur);
            cur = word;
        } else {
            cur = trial;
        }
        i = j + 1;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

bool restart_pending() {
    return prefs::get().quality != prefs::at_startup().quality ||
           prefs::get().gl_thread != prefs::at_startup().gl_thread ||
           prefs::get().direct_memory != prefs::at_startup().direct_memory;
}
}  // namespace

bool combo_pressed(const InputState& now, const InputState& before) {
    const unsigned c = platform::BTN_ZL | platform::BTN_ZR | platform::BTN_MINUS;
    return (now.buttons & c) == c && (before.buttons & c) != c;
}

bool is_open() { return g_open; }

void open() {
    if (g_open) return;
    build_items();
    g_open = true;
    g_sel = std::clamp(g_sel, 0, (int)g_items.size() - 1);
    for (auto& r : g_rep) r = Repeat{};
    g_touching = false;
    LOGI("debug menu opened");
}

void close() {
    if (!g_open) return;
    g_open = false;
    LOGI("debug menu closed");
}

void update(const InputState& s, const InputState& prev) {
    if (!g_open) return;
    s64 now = now_ms();
    auto pressed = [&](unsigned b) { return (s.buttons & b) && !(prev.buttons & b); };
    if (pressed(platform::BTN_B) || pressed(platform::BTN_MINUS) || pressed(platform::BTN_PLUS)) {
        close();
        return;
    }
    const float t = 0.6f;
    bool dir[4] = {
        (s.buttons & platform::BTN_UP) || s.ly < -t,
        (s.buttons & platform::BTN_DOWN) || s.ly > t,
        (s.buttons & platform::BTN_LEFT) || s.lx < -t,
        (s.buttons & platform::BTN_RIGHT) || s.lx > t,
    };
    int n = (int)g_items.size();
    if (g_rep[0].fire(dir[0], now)) g_sel = (g_sel + n - 1) % n;
    if (g_rep[1].fire(dir[1], now)) g_sel = (g_sel + 1) % n;
    if (g_rep[2].fire(dir[2], now)) step(g_items[g_sel], -1, false);
    if (g_rep[3].fire(dir[3], now)) step(g_items[g_sel], +1, false);
    if (pressed(platform::BTN_A)) activate(g_items[g_sel]);
    if (!g_open) return;

    // Touch: tap a row to pick it; on a setting, the left/right half of the
    // value changes it down/up; a tap outside the panel closes the menu.
    if (s.touch && !g_touching) {
        g_touching = true;
        g_touch_x = s.touch_x;
        g_touch_y = s.touch_y;
    } else if (!s.touch && g_touching) {
        g_touching = false;
        float x = g_touch_x, y = g_touch_y;
        if (!g_panel.in(x, y)) {
            close();
            return;
        }
        for (int i = 0; i < (int)g_rows.size() && i < n; i++) {
            if (!g_rows[i].in(x, y)) continue;
            g_sel = i;
            Item& it = g_items[i];
            if (it.kind != Kind::Setting) activate(it);
            else if (x >= g_value_x0) step(it, x < g_value_mid ? -1 : +1, false);
            break;
        }
    }
}

void draw(int W, int H) {
    if (!g_open) return;
    using namespace overlay;
    if (!begin(W, H)) return;
    const float u = H / 720.f;
    // The game's own palette: dark slate panels, white text, its accent red.
    const Color red = kGameRed, bright_red{0.96f, 0.22f, 0.24f, 1.f}, white{1.f, 1.f, 1.f, 1.f},
                gray{0.62f, 0.62f, 0.64f, 1.f}, orange{1.f, 0.6f, 0.2f, 1.f}, dim_text{0.85f, 0.85f, 0.87f, 1.f};
    const bool clocks = platform::clocks_supported();

    rect(0, 0, (float)W, (float)H, {0.f, 0.f, 0.f, 0.5f});  // dim the game behind

    const float pw = std::min(900 * u, W - 20 * u), row_h = 34 * u;
    const int n = (int)g_items.size();
    const float ph = 70 * u + n * row_h + 16 * u + 150 * u;
    const float x0 = (W - pw) / 2, y0 = std::max(8 * u, (H - ph) / 2), x1 = x0 + pw, y1 = y0 + ph;
    g_panel = {x0, y0, x1, y1};
    rect(x0, y0, x1, y1, {0.09f, 0.10f, 0.11f, 0.96f});
    frame(x0, y0, x1, y1, std::max(1.f, std::round(u)), {1.f, 1.f, 1.f, 0.10f});
    rect(x0, y0, x1, y0 + 5 * u, red);
    text(x0 + 24 * u, y0 + 44 * u, 28 * u, "PORT SETTINGS", white);
    text(x0 + 24 * u + text_width("PORT SETTINGS", 28 * u) + 14 * u, y0 + 44 * u, 17 * u, "debug menu", gray);
    text(x1 - 24 * u, y0 + 42 * u, 15 * u, "Up/Down: choose    Left/Right, A: change    B: close", gray,
         Align::Right);

    const float label_x = x0 + 28 * u;
    const float value_mid = x0 + pw * 0.71f, value_half = pw * 0.2f;
    g_value_x0 = value_mid - value_half - 30 * u;
    g_value_mid = value_mid;
    g_rows.assign(n, Rect{});
    float y = y0 + 64 * u;
    for (int i = 0; i < n; i++) {
        const Item& it = g_items[i];
        if (it.kind != Kind::Setting && (i == 0 || g_items[i - 1].kind == Kind::Setting)) {
            rect(x0 + 20 * u, y + 6 * u, x1 - 20 * u, y + 7 * u, {1.f, 1.f, 1.f, 0.12f});  // separator
            y += 12 * u;
        }
        float ry0 = y, ry1 = y + row_h - 3 * u;
        g_rows[i] = {x0 + 10 * u, ry0, x1 - 10 * u, ry1};
        bool sel = i == g_sel;
        if (sel) {
            // Selected row: red-tinted band with a solid red edge on the left.
            const float sx0 = x0 + 12 * u, sx1 = x1 - 12 * u;
            rect(sx0, ry0, sx1, ry1, {red.r, red.g, red.b, 0.22f});
            rect(sx0, ry0, sx1, ry0 + std::max(1.f, std::round(u)), {red.r, red.g, red.b, 0.7f});
            rect(sx0, ry1 - std::max(1.f, std::round(u)), sx1, ry1, {red.r, red.g, red.b, 0.7f});
            rect(sx0, ry0, sx0 + 5 * u, ry1, red);
        }
        float base = ry0 + 25 * u;
        text(label_x, base, 20 * u, it.label, sel ? white : dim_text);
        if (it.restart) {
            text(label_x + text_width(it.label, 20 * u) + 10 * u, base, 14 * u, "NEXT START", gray);
        } else if (it.switch_only && !clocks) {
            text(label_x + text_width(it.label, 20 * u) + 10 * u, base, 14 * u, "SWITCH ONLY", gray);
        }
        if (it.kind == Kind::Setting) {
            int idx = current_index(it);
            std::string v;
            if (idx >= 0) v = it.choices[idx].text;
            else if (it.field_int == &Values::scene_scale) v = std::to_string(get_value(it)) + "%";
            else v = it.field_int && (it.field_int == &Values::cpu_khz || it.field_int == &Values::gpu_khz ||
                                     it.field_int == &Values::mem_khz)
                         ? mhz_text(get_value(it))
                         : std::to_string(get_value(it));
            Color vc = (it.switch_only && !clocks) ? gray : white;
            text(value_mid, base, 20 * u, v, vc, Align::Center);
            if (sel) {
                bool can_left = idx != 0, can_right = idx != (int)it.choices.size() - 1;
                text(value_mid - value_half, base, 22 * u, "<", can_left ? bright_red : gray, Align::Center);
                text(value_mid + value_half, base, 22 * u, ">", can_right ? bright_red : gray, Align::Center);
            }
        }
        y += row_h;
    }

    // Help for the selected row, then side effects of the chosen value.
    const Item& cur = g_items[g_sel];
    float hy = y + 22 * u;
    const float help_px = 17 * u, help_w = pw - 56 * u;
    std::vector<std::pair<std::string, Color>> help;
    std::string help_text = cur.help;
    if (cur.kind == Kind::Setting) {
        // "Recommended: ..." from the defaults
        Values d = prefs::defaults();
        int dv = cur.field_int ? d.*cur.field_int : (d.*cur.field_bool ? 1 : 0);
        for (auto& c : cur.choices)
            if (c.value == dv) help_text += "  Recommended: " + c.text + ".";
    }
    for (auto& l : wrap(help_text, help_px, help_w)) help.push_back({l, dim_text});
    if (cur.kind == Kind::Setting) {
        int idx = current_index(cur);
        if (idx >= 0 && !cur.choices[idx].note.empty())
            for (auto& l : wrap(cur.choices[idx].note, help_px, help_w)) help.push_back({l, orange});
        if (cur.switch_only && !clocks) help.push_back({"Only has an effect on the Switch.", gray});
        if (cur.field_int == &Values::scene_scale && perf::startup_tier() != prefs::kQualityHighest)
            help.push_back({"No effect at the detail level the game is running now.", gray});
    }
    if (now_ms() < g_flash_until) help.push_back({g_flash, white});
    else if (restart_pending())
        help.push_back({"Settings marked NEXT START apply when the game starts again (close it from HOME).", orange});
    for (size_t i = 0; i < help.size() && i < 5; i++) text(label_x, hy + i * 22 * u, help_px, help[i].first, help[i].second);

    // Live numbers, so the effect of a change can be seen right away.
    const auto& st = perf::stats();
    char line[160];
    std::snprintf(line, sizeof line, "Now: %.1f FPS   frame %.1f ms   game %.1f ms   GL calls %.0f", st.fps,
                  st.frame_ms, st.tick_ms, st.gl_calls);
    text(label_x, y1 - 16 * u, 16 * u, line, gray);
    text(x1 - 24 * u, y1 - 16 * u, 15 * u, "Open/close: ZL + ZR + Minus", gray, Align::Right);
    end();
}

}  // namespace game::debug_menu
