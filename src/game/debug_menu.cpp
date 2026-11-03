#include "game/debug_menu.h"

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/memory.h"
#include "game/config.h"
#include "game/motion.h"
#include "game/overlay.h"
#include "game/perf.h"
#include "game/prefs.h"
#include "hle/hle.h"
#include "platform/thread.h"

#ifndef NFSMW_VERSION
#define NFSMW_VERSION "0.0.0"
#endif

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
enum class Kind { Setting, Info, Restore, Close };
struct Item {
    Kind kind;
    std::string label;
    int Values::*field_int = nullptr;
    bool Values::*field_bool = nullptr;
    float Values::*field_float = nullptr;
    std::vector<Choice> choices;
    bool restart = false;      // takes effect at the next start
    bool switch_only = false;  // console clocks
    std::string help;
    std::string value;         // Info rows: the text on the right
};
// The menu's pages; L / R (or a tap on a tab) switch between them.
enum Page { kPerformance, kGraphics, kControls, kInfo, kPages };
const char* const kPageNames[kPages] = {"PERFORMANCE", "GRAPHICS", "CONTROLS & SOUND", "INFORMATION"};

bool g_open = false;
int g_page = kPerformance;
int g_sel = 0;
std::vector<Item> g_items;
std::string g_flash;  // short confirmation under the help text
s64 g_flash_until = 0;
s64 g_info_next = 0;  // information page: next refresh of the system figures
platform::SystemStatus g_sys;
bool g_have_sys = false;
const auto g_session_start = std::chrono::steady_clock::now();

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
Rect g_tabs[kPages];
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
std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char b[256];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

// ---- pages ------------------------------------------------------------------------
void performance_items(std::vector<Item>& out, bool docked) {
    Item fps{Kind::Setting, "Frame rate limit"};
    fps.field_int = &Values::fps_limit;
    fps.choices = {{30, "30 FPS", ""},
                   {60, "60 FPS", "Only smoother when the game really reaches 60; otherwise motion gets uneven."},
                   {0, "Off", "Frames come as fast as they can, so motion is uneven."}};
    fps.help = "The game is made for 30 FPS. Below 30 it does not skip ahead but slows down, so a steady 30 "
               "matters most.";
    out.push_back(fps);

    Item vs{Kind::Setting, "VSync"};
    vs.field_bool = &Values::vsync;
    vs.choices = {{1, "On", ""}, {0, "Off", "The picture can tear (a visible line where two frames meet)."}};
    vs.help = "Shows each frame at the screen's refresh. Off can lower input delay a little, but tears.";
    out.push_back(vs);

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
    out.push_back(cpu);

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
               "clock Nintendo uses) and the normal clock when docked. Anti-aliasing needs more of it.";
    out.push_back(gpu);

    Item mem{Kind::Setting, "Memory clock"};
    mem.field_int = &Values::mem_khz;
    mem.switch_only = true;
    mem.choices = {{prefs::kClockStock, docked ? "Stock (1600 MHz)" : "Stock (1331.2 MHz)", ""},
                   {1331200, "1331.2 MHz", "Slower memory: a lower frame rate in busy scenes."},
                   {1600000, "1600 MHz", docked ? "" : "Above the normal handheld clock: more battery use."}};
    mem.help = "1600 MHz is the docked memory clock; Nintendo also uses it in handheld mode.";
    out.push_back(mem);

    Item lb{Kind::Setting, "CPU boost while loading"};
    lb.field_bool = &Values::loading_boost;
    lb.switch_only = true;
    lb.choices = {{1, "On", ""}, {0, "Off", "Loading screens and cars in the garage take longer."}};
    lb.help = "While the game loads (loading screens, a car in the garage) the CPU runs at 1785 MHz, as official "
              "Switch games do, then goes back to the clock above. Most of a load is CPU work.";
    out.push_back(lb);

    Item gt{Kind::Setting, "GL on its own core"};
    gt.field_bool = &Values::gl_thread;
    gt.restart = true;
    gt.choices = {{1, "On", ""},
                  {0, "Off", "The game thread then also does the graphics driver's work: a lower frame rate."}};
    gt.help = "Runs the game's graphics calls on a separate CPU core, so the game code does not wait for the "
              "driver or for the screen refresh.";
    out.push_back(gt);

    Item dm{Kind::Setting, "Direct memory access"};
    dm.field_bool = &Values::direct_memory;
    dm.restart = true;
    dm.switch_only = true;
    dm.choices = {{1, "On", ""},
                  {0, "Off", "Every memory access of the game code goes through a lookup table: slower."}};
    dm.help = "The translated game code reads and writes its memory directly instead of through a lookup "
              "table. Turn it off only if the game crashes.";
    out.push_back(dm);
}

void graphics_items(std::vector<Item>& out) {
    Item q{Kind::Setting, "Detail level"};
    q.field_int = &Values::quality;
    q.restart = true;
    q.choices = {{prefs::kQualityHighest, "Highest", "Adds a speed-blur pass; needs the most CPU time and GL calls."},
                 {prefs::kQualityHigh, "High", "Like Highest without the speed blur; about as much CPU time."},
                 {prefs::kQualityMedium, "Medium", ""},
                 {prefs::kQualityLow, "Low", ""}};
    q.help = std::string("The game's own detail level, the one it gives phones by their speed. Medium and Low draw "
                         "about a quarter fewer objects per frame, which saves CPU time. Running now: ") +
             perf::tier_name(perf::startup_tier()) + ".";
    out.push_back(q);

    Item fx{Kind::Setting, "Phone effects"};
    fx.field_int = &Values::effects;
    fx.restart = true;
    fx.choices = {{prefs::kEffectsGame, "Off", ""},
                  {prefs::kEffectsHigh, "High-end phones", ""},
                  {prefs::kEffectsHighest, "Top phones", "Adds detail level Highest's live car reflections and brake "
                                                         "lights: about a quarter more CPU time per frame, like High."}};
    fx.help = "Effects the game gives only fast phones: haze in the distance, wet shiny roads, reflections on cars, "
              "smoke, engine echo in tunnels. Added to detail level Medium without detail level High's extra objects, "
              "which are what cost CPU time.";
    out.push_back(fx);

    Item aa{Kind::Setting, "Anti-aliasing"};
    aa.field_int = &Values::msaa;
    aa.choices = {{0, "Off", ""},
                  {2, "2x MSAA", "Costs GPU time; watch the frame rate (a higher GPU clock helps)."},
                  {4, "4x MSAA", "Costs more GPU time, most of all docked; watch the frame rate."}};
    aa.help = "Smoother edges on cars, roads and buildings. The game has this for phones but it was switched "
              "off on Android; the port does it itself. No effect at detail level Highest, which draws the race "
              "off-screen.";
    out.push_back(aa);

    Item af{Kind::Setting, "Texture filtering"};
    af.field_int = &Values::anisotropy;
    af.choices = {{1, "Standard", ""}, {2, "2x anisotropic", ""}, {4, "4x anisotropic", ""},
                  {8, "8x anisotropic", ""}, {16, "16x anisotropic", "The most the GPU can do; a little more GPU time."}};
    af.help = "Keeps textures sharp when seen at a slant: the road ahead, building walls, the track on the map. "
              "Cheap for the GPU.";
    out.push_back(af);

    Item res{Kind::Setting, "3D resolution"};
    res.field_int = &Values::scene_scale;
    int ow, oh;
    perf::output_size(ow, oh);  // the real screen (it changes when docking / undocking)
    for (int pct : {100, 90, 80, 75, 67, 50}) {
        res.choices.push_back({pct, fmt("%d%%  (%dx%d)", pct, (int)std::lround(ow * pct / 100.0),
                                        (int)std::lround(oh * pct / 100.0)),
                               pct <= 67 ? "Noticeably blurrier 3D scene." : ""});
    }
    res.help = "Resolution of the race scene. Lower = less work for the GPU; menus and the HUD stay sharp. "
               "Works at detail level Highest, the level that draws the scene off-screen for its speed blur.";
    out.push_back(res);

    Item ov{Kind::Setting, "Frame-rate counter"};
    ov.field_int = &Values::overlay;
    ov.choices = {{prefs::kOverlayOff, "Off", ""}, {prefs::kOverlayFps, "FPS", ""}, {prefs::kOverlayFull, "Full", ""}};
    ov.help = "Shows the frame rate in the top-left corner. Full adds frame times, clocks and resolution.";
    out.push_back(ov);
}

void controls_items(std::vector<Item>& out) {
    Item gy{Kind::Setting, "Tilt steering"};
    gy.field_bool = &Values::gyro_steering;
    gy.choices = {{0, "Off", ""}, {1, "On", ""}};
    gy.help = "Steer by tilting the console (or the controller) like a steering wheel; the left stick still "
              "steers and wins while it is pushed. Also in the game: Options > Controls > TILT TO STEER.";
    out.push_back(gy);

    Item ga{Kind::Setting, "Tilt for full lock"};
    ga.field_float = &Values::gyro_max_angle;
    for (int deg : {15, 20, 25, 30, 40, 50, 60})
        ga.choices.push_back({deg, fmt("%d degrees", deg), deg <= 15 ? "Very sensitive: small hand movements steer." : ""});
    ga.help = "How far to tilt for full steering lock. Smaller = more sensitive.";
    out.push_back(ga);

    Item gi{Kind::Setting, "Invert tilt"};
    gi.field_bool = &Values::gyro_invert;
    gi.choices = {{0, "Off", ""}, {1, "On", ""}};
    gi.help = "Turn on if the car steers the wrong way when you tilt.";
    out.push_back(gi);

    Item snd{Kind::Setting, "Sound volume"};
    snd.field_int = &Values::sound_volume;
    for (int pct : {100, 90, 80, 70, 60, 50})
        snd.choices.push_back({pct, std::to_string(pct) + "%",
                               pct > 70 ? "Loud moments are cut off and crackle (more the higher)." : ""});
    snd.help = "Volume of the game's sound mix. The game mixes so loud that loud moments (engines, crashes, "
               "music) are cut off and crackle; a lower value leaves room for them. Turn the console's volume up "
               "instead.";
    out.push_back(snd);
}

std::string clock_line() {
    u32 c = 0, g = 0, m = 0;
    if (!perf::clocks_now(c, g, m)) return "not available on this system";
    auto mhz = [](u32 hz) { return hz % 1000000 ? fmt("%.1f", hz / 1e6) : fmt("%u", hz / 1000000); };
    return "CPU " + mhz(c) + "   GPU " + mhz(g) + "   memory " + mhz(m) + " MHz" +
           (perf::boosting_now() ? "   (loading boost)" : "");
}

void info_items(std::vector<Item>& out) {
    auto row = [&](const std::string& label, const std::string& value) {
        Item it{Kind::Info, label};
        it.value = value;
        out.push_back(it);
    };
    const s64 now = now_ms();
    if (now >= g_info_next) {  // the system services once a second
        g_have_sys = platform::system_status(g_sys);
        g_info_next = now + 1000;
    }
    int ow, oh;
    perf::output_size(ow, oh);
    const float sc = hle::gl_scene_scale();
    row("Version", std::string("port ") + NFSMW_VERSION + ", game 1.3.128 (Android), detail " +
                       perf::tier_name(perf::startup_tier()));
    const int aa = hle::gl_msaa_samples(), af = hle::gl_anisotropy();
    row("Screen", fmt("%s %dx%d   3D %dx%d   AA %s   filtering %s", platform::is_docked() ? "docked" : "handheld", ow,
                      oh, (int)std::lround(ow * sc), (int)std::lround(oh * sc),
                      aa ? fmt("%dx", aa).c_str() : "off", af > 1 ? fmt("%dx", af).c_str() : "standard"));
    const auto& st = perf::stats();
    row("Frame rate", fmt("%.1f FPS   frame %.1f ms   game %.1f ms   %.0f GL calls", st.fps, st.frame_ms, st.tick_ms,
                          st.gl_calls));
    row("Clocks", clock_line());
    if (g_have_sys && g_sys.temps)
        row("Temperature", fmt("SoC %.0f C   board %.0f C", g_sys.soc_c, g_sys.pcb_c));
    else
        row("Temperature", "not available on this system");
    if (g_have_sys && g_sys.battery)
        row("Battery", fmt("%d%%%s", g_sys.battery_pct, g_sys.charging ? "   charging" : ""));
    else
        row("Battery", "not available on this system");
    row("Memory", fmt("game %.0f MiB in use", mem::heap_bytes_in_use() / 1048576.0));
    row("Threads", fmt("%d running (the Switch allows a game 96)", platform::live_threads()));
    std::string loads;
    for (const perf::Load& l : perf::recent_loads()) {
        if (!loads.empty()) loads += "   ";
        loads += fmt("%s %.1f s%s", l.screen ? "screen" : "car", l.secs, l.boosted ? "*" : "");
    }
    row("Last loads", loads.empty() ? std::string("none yet") : loads + (loads.find('*') != std::string::npos
                                                                          ? "   (* boosted)"
                                                                          : ""));
    const long secs = (long)std::chrono::duration<double>(std::chrono::steady_clock::now() - g_session_start).count();
    row("Session", fmt("%ld h %02ld min, %ld frames", secs / 3600, secs / 60 % 60, perf::frames_shown()));
}

void build_items() {
    g_items.clear();
    switch (g_page) {
    case kPerformance: performance_items(g_items, platform::is_docked()); break;
    case kGraphics: graphics_items(g_items); break;
    case kControls: controls_items(g_items); break;
    case kInfo: info_items(g_items); break;
    }
    if (g_page != kInfo) {
        Item rs{Kind::Restore, "Restore recommended settings"};
        rs.help = "Every setting on the PERFORMANCE and GRAPHICS pages, and the sound volume, back to the "
                  "recommended values. Tilt steering stays as it is.";
        g_items.push_back(rs);
    }
    Item cl{Kind::Close, "Close"};
    cl.help = g_page == kInfo ? "Live figures; the system's are read once a second."
                              : "Settings are saved as you change them (settings.ini, next to the .nro).";
    g_items.push_back(cl);
}

int get_value(const Item& it) {
    auto& v = prefs::get();
    if (it.field_int) return v.*it.field_int;
    if (it.field_bool) return (v.*it.field_bool) ? 1 : 0;
    if (it.field_float) return (int)std::lround(v.*it.field_float);
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
    if (it.field_bool == &Values::gyro_steering || it.field_float == &Values::gyro_max_angle ||
        it.field_bool == &Values::gyro_invert)
        motion::reset();
    if (!it.restart) perf::apply();
}

void set_index(Item& it, int idx) {
    if (it.choices.empty()) return;
    idx = std::clamp(idx, 0, (int)it.choices.size() - 1);
    int v = it.choices[idx].value;
    auto& vals = prefs::get();
    if (it.field_int) vals.*it.field_int = v;
    if (it.field_bool) vals.*it.field_bool = v != 0;
    if (it.field_float) vals.*it.field_float = (float)v;
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

void go_page(int page) {
    page = (page % kPages + kPages) % kPages;
    if (page == g_page) return;
    g_page = page;
    g_sel = 0;
    g_info_next = 0;
    build_items();
}

void activate(Item& it) {
    switch (it.kind) {
    case Kind::Setting: step(it, +1, true); break;
    case Kind::Info: break;
    case Kind::Restore: {
        auto& v = prefs::get();
        Values d = prefs::defaults();
        // Performance, graphics and sound; tilt steering stays as it is.
        v.fps_limit = d.fps_limit;
        v.vsync = d.vsync;
        v.scene_scale = d.scene_scale;
        v.quality = d.quality;
        v.cpu_khz = d.cpu_khz;
        v.gpu_khz = d.gpu_khz;
        v.mem_khz = d.mem_khz;
        v.loading_boost = d.loading_boost;
        v.anisotropy = d.anisotropy;
        v.msaa = d.msaa;
        v.effects = d.effects;
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
    return prefs::get().quality != prefs::at_startup().quality || prefs::get().effects != prefs::at_startup().effects ||
           prefs::get().gl_thread != prefs::at_startup().gl_thread ||
           prefs::get().direct_memory != prefs::at_startup().direct_memory;
}

// The most rows any page has (the panel keeps one size on every page).
int max_rows() {
    static int n = 0;
    if (n) return n;
    std::vector<Item> v;
    performance_items(v, false);
    n = std::max<int>(n, (int)v.size() + 2);
    v.clear();
    graphics_items(v);
    n = std::max<int>(n, (int)v.size() + 2);
    v.clear();
    controls_items(v);
    n = std::max<int>(n, (int)v.size() + 2);
    n = std::max(n, 11 + 1);  // information: 10 rows and Close
    return n;
}
}  // namespace

bool combo_pressed(const InputState& now, const InputState& before) {
    const unsigned c = platform::BTN_ZL | platform::BTN_ZR | platform::BTN_MINUS;
    return (now.buttons & c) == c && (before.buttons & c) != c;
}

bool is_open() { return g_open; }

void open() {
    if (g_open) return;
    g_info_next = 0;
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
    if (pressed(platform::BTN_L)) go_page(g_page - 1);
    if (pressed(platform::BTN_R)) go_page(g_page + 1);
    if (g_page == kInfo) build_items();  // live figures
    const float t = 0.6f;
    bool dir[4] = {
        (s.buttons & platform::BTN_UP) || s.ly < -t,
        (s.buttons & platform::BTN_DOWN) || s.ly > t,
        (s.buttons & platform::BTN_LEFT) || s.lx < -t,
        (s.buttons & platform::BTN_RIGHT) || s.lx > t,
    };
    int n = (int)g_items.size();
    auto selectable = [&](int i) { return g_items[i].kind != Kind::Info; };
    auto move = [&](int d) {
        for (int k = 0; k < n; k++) {
            g_sel = (g_sel + d + n) % n;
            if (selectable(g_sel)) break;
        }
    };
    if (!selectable(std::clamp(g_sel, 0, n - 1))) {
        g_sel = std::clamp(g_sel, 0, n - 1);
        move(+1);
    }
    if (g_rep[0].fire(dir[0], now)) move(-1);
    if (g_rep[1].fire(dir[1], now)) move(+1);
    if (g_rep[2].fire(dir[2], now)) step(g_items[g_sel], -1, false);
    if (g_rep[3].fire(dir[3], now)) step(g_items[g_sel], +1, false);
    if (pressed(platform::BTN_A)) activate(g_items[g_sel]);
    if (!g_open) return;

    // Touch: tap a tab to switch pages, a row to pick it; on a setting, the
    // left/right half of the value changes it down/up; a tap outside the panel
    // closes the menu.
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
        for (int p = 0; p < kPages; p++)
            if (g_tabs[p].in(x, y)) {
                go_page(p);
                return;
            }
        for (int i = 0; i < (int)g_rows.size() && i < n; i++) {
            if (!g_rows[i].in(x, y) || !selectable(i)) continue;
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

    const float pw = std::min(940 * u, W - 20 * u), row_h = 32 * u, tab_h = 36 * u;
    const int n = (int)g_items.size();
    const float ph = std::min(H - 16 * u, 64 * u + tab_h + max_rows() * row_h + 12 * u + 120 * u);
    const float x0 = (W - pw) / 2, y0 = std::max(8 * u, (H - ph) / 2), x1 = x0 + pw, y1 = y0 + ph;
    g_panel = {x0, y0, x1, y1};
    rect(x0, y0, x1, y1, {0.09f, 0.10f, 0.11f, 0.96f});
    frame(x0, y0, x1, y1, std::max(1.f, std::round(u)), {1.f, 1.f, 1.f, 0.10f});
    rect(x0, y0, x1, y0 + 5 * u, red);
    text(x0 + 24 * u, y0 + 42 * u, 27 * u, "PORT SETTINGS", white);
    text(x0 + 24 * u + text_width("PORT SETTINGS", 27 * u) + 14 * u, y0 + 42 * u, 17 * u, "debug menu", gray);
    text(x1 - 24 * u, y0 + 40 * u, 15 * u, "Open/close: ZL + ZR + Minus", gray, Align::Right);

    // Tabs
    {
        const float ty0 = y0 + 56 * u, ty1 = ty0 + tab_h - 6 * u, tw = (pw - 40 * u) / (float)kPages;
        for (int p = 0; p < kPages; p++) {
            const float tx0 = x0 + 20 * u + p * tw, tx1 = tx0 + tw - 6 * u;
            g_tabs[p] = {tx0, ty0, tx1, ty1};
            const bool on = p == g_page;
            rect(tx0, ty0, tx1, ty1, on ? Color{red.r, red.g, red.b, 0.85f} : Color{1.f, 1.f, 1.f, 0.06f});
            text((tx0 + tx1) / 2, ty0 + 21 * u, 15 * u, kPageNames[p], on ? white : gray, Align::Center);
        }
    }

    const float label_x = x0 + 28 * u;
    const float value_mid = x0 + pw * 0.69f, value_half = pw * 0.2f;
    g_value_x0 = value_mid - value_half - 30 * u;
    g_value_mid = value_mid;
    g_rows.assign(n, Rect{});
    float y = y0 + 56 * u + tab_h + 4 * u;
    for (int i = 0; i < n; i++) {
        const Item& it = g_items[i];
        const bool action = it.kind == Kind::Restore || it.kind == Kind::Close;
        if (action && (i == 0 || !(g_items[i - 1].kind == Kind::Restore || g_items[i - 1].kind == Kind::Close))) {
            rect(x0 + 20 * u, y + 5 * u, x1 - 20 * u, y + 6 * u, {1.f, 1.f, 1.f, 0.12f});  // separator
            y += 10 * u;
        }
        float ry0 = y, ry1 = y + row_h - 3 * u;
        g_rows[i] = {x0 + 10 * u, ry0, x1 - 10 * u, ry1};
        bool sel = i == g_sel && it.kind != Kind::Info;
        if (sel) {
            // Selected row: red-tinted band with a solid red edge on the left.
            const float sx0 = x0 + 12 * u, sx1 = x1 - 12 * u;
            rect(sx0, ry0, sx1, ry1, {red.r, red.g, red.b, 0.22f});
            rect(sx0, ry0, sx1, ry0 + std::max(1.f, std::round(u)), {red.r, red.g, red.b, 0.7f});
            rect(sx0, ry1 - std::max(1.f, std::round(u)), sx1, ry1, {red.r, red.g, red.b, 0.7f});
            rect(sx0, ry0, sx0 + 5 * u, ry1, red);
        }
        float base = ry0 + 23 * u;
        if (it.kind == Kind::Info) {
            text(label_x, base, 17 * u, it.label, gray);
            text(label_x + 150 * u, base, 17 * u, it.value, white);
            y += row_h;
            continue;
        }
        text(label_x, base, 19 * u, it.label, sel ? white : dim_text);
        if (it.restart) {
            text(label_x + text_width(it.label, 19 * u) + 10 * u, base, 13 * u, "NEXT START", gray);
        } else if (it.switch_only && !clocks) {
            text(label_x + text_width(it.label, 19 * u) + 10 * u, base, 13 * u, "SWITCH ONLY", gray);
        }
        if (it.kind == Kind::Setting) {
            int idx = current_index(it);
            std::string v;
            if (idx >= 0) v = it.choices[idx].text;
            else if (it.field_int == &Values::scene_scale) v = std::to_string(get_value(it)) + "%";
            else if (it.field_float) v = std::to_string(get_value(it)) + " degrees";
            else v = it.field_int && (it.field_int == &Values::cpu_khz || it.field_int == &Values::gpu_khz ||
                                     it.field_int == &Values::mem_khz)
                         ? mhz_text(get_value(it))
                         : std::to_string(get_value(it));
            Color vc = (it.switch_only && !clocks) ? gray : white;
            text(value_mid, base, 19 * u, v, vc, Align::Center);
            if (sel) {
                bool can_left = idx != 0, can_right = idx != (int)it.choices.size() - 1;
                text(value_mid - value_half, base, 21 * u, "<", can_left ? bright_red : gray, Align::Center);
                text(value_mid + value_half, base, 21 * u, ">", can_right ? bright_red : gray, Align::Center);
            }
        }
        y += row_h;
    }

    // Help for the selected row, then side effects of the chosen value.
    const Item& cur = g_items[std::clamp(g_sel, 0, n - 1)];
    float hy = std::max(y + 20 * u, y1 - 112 * u);
    const float help_px = 16 * u, help_w = pw - 56 * u;
    std::vector<std::pair<std::string, Color>> help;
    std::string help_text = cur.help;
    if (cur.kind == Kind::Setting) {
        // "Recommended: ..." from the defaults
        Values d = prefs::defaults();
        int dv = cur.field_int ? d.*cur.field_int
                 : cur.field_bool ? (d.*cur.field_bool ? 1 : 0)
                                  : (int)std::lround(d.*cur.field_float);
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
        if (cur.field_int == &Values::effects && perf::startup_tier() != prefs::kQualityMedium &&
            !(perf::startup_tier() == prefs::kQualityHigh && prefs::get().effects == prefs::kEffectsHighest))
            help.push_back({"Has an effect at detail level Medium (Top phones: also at High).", gray});
        if (cur.field_int == &Values::msaa && prefs::get().msaa > 1 && hle::gl_msaa_samples() == 0)
            help.push_back({"Not available with this GPU driver.", orange});
    }
    if (now_ms() < g_flash_until) help.push_back({g_flash, white});
    else if (restart_pending())
        help.push_back({"Settings marked NEXT START apply when the game starts again (close it from HOME).", orange});
    for (size_t i = 0; i < help.size() && i < 4; i++) text(label_x, hy + i * 21 * u, help_px, help[i].first, help[i].second);

    // Live numbers, so the effect of a change can be seen right away.
    const auto& st = perf::stats();
    text(label_x, y1 - 14 * u, 15 * u,
         fmt("Now: %.1f FPS   frame %.1f ms   game %.1f ms   GL calls %.0f", st.fps, st.frame_ms, st.tick_ms,
             st.gl_calls),
         gray);
    text(x1 - 24 * u, y1 - 14 * u, 15 * u, "L/R: page   Left/Right: change   B: close", gray, Align::Right);
    end();
}

}  // namespace game::debug_menu
