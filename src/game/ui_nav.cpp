// See ui_nav.h. Engine facts used here (NFS:MW 1.3.128, libapp.so):
//
//  im::scene2d_new::Node
//    +0x00 vtable          +0x18 flags (0x10 = hidden)
//    +0x38 parent Node*    +0x3c colour (RGBA, alpha in the top byte)
//    +0x44 rect x0, y0, x1, y1 (floats, local units = pixels)
//    vtable slot 15  HitTest(int x, int y)  (local coordinates)
//    vtable slot 33  IsInteractive()
//    vtable slot 47  PreRender(Renderer&)   pushes the node's transform; returns
//                    false when the node is not drawn
//  Renderer: matrix stack of 4x4 column-major floats, top = *(renderer+0x174) - 64
//
// These were read from the game's code; the RTTI names (e.g.
// N2im11scene2d_new7layouts6ButtonE) are only used to find the classes.
#include "game/ui_nav.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include "core/cpu.h"
#include "core/elf.h"
#include "core/hook.h"
#include "core/memory.h"
#include "core/thunks.h"
#include "game/overlay.h"
#include "game/rtti.h"

namespace game::ui {

bool g_delivering_key = false;

namespace {

constexpr u32 kSlotHitTest = 15;
constexpr u32 kSlotIsInteractive = 33;
constexpr u32 kSlotPreRender = 47;
constexpr u32 kNodeParent = 0x38;
constexpr u32 kNodeRect = 0x44;
constexpr u32 kRendererMatrixEnd = 0x174;
constexpr u32 kRendererColorBegin = 0x130;  // vector<u32>: begin, end
// Clip stack (scroll areas, lists): end pointer; the top entry ends with
// {x, y, w, h (floats, screen pixels), shared_ptr (8 bytes), bool active}.
constexpr u32 kRendererClipEnd = 0x144;

bool g_active = false;
elf::Module* g_mod = nullptr;
std::vector<Button> g_rec, g_last;
std::unordered_map<u32, const char*> g_cls_of_vtable;  // hooked vtables -> class name
int g_order = 0;

bool is_code(u32 a) { return rtti::is_code(g_mod, a); }

float f32(u32 a) {
    float f;
    u32 v = mem::r32(a);
    std::memcpy(&f, &v, 4);
    return f;
}

// A node's name from the layout ("btn_continue", "button", ...): +0x20 points
// to a string (directly, or to a string object whose first word is the text).
std::string node_name(u32 node) {
    if (node < mem::HEAP_BASE) return {};
    u32 w = mem::r32(node + 0x20);
    for (u32 p : {w, w >= mem::HEAP_BASE && w < mem::g_size - 16 ? mem::r32(w) : 0u}) {
        if (p < mem::HEAP_BASE || p >= mem::g_size - 64) continue;
        const char* c = mem::str(p);
        int n = 0;
        while (n < 48 && c[n] >= 32 && c[n] < 127) n++;
        if (n >= 1 && c[n] == 0) return std::string(c, n);
    }
    return {};
}

// Nearest ancestor that scrolls (class name contains Scroll/List/Swipe).
std::unordered_map<u32, bool> g_scroll_vtable;
u32 scroll_ancestor(u32 node) {
    u32 n = mem::r32(node + kNodeParent);
    for (int depth = 0; n >= mem::HEAP_BASE && n < mem::g_size && depth < 32; depth++) {
        u32 vt = mem::r32(n);
        auto it = g_scroll_vtable.find(vt);
        bool scrolls;
        if (it != g_scroll_vtable.end()) {
            scrolls = it->second;
        } else {
            std::string c = rtti::class_name(g_mod, n);
            scrolls = c.find("Scroll") != std::string::npos || c.find("Swipe") != std::string::npos ||
                      c.find("List") != std::string::npos;
            g_scroll_vtable[vt] = scrolls;
        }
        if (scrolls) return n;
        n = mem::r32(n + kNodeParent);
    }
    return 0;
}

void record(u32 node, u32 renderer) {
    u32 top = mem::r32(renderer + kRendererMatrixEnd) - 64;
    if (top < mem::HEAP_BASE) return;
    float m[16];
    for (int i = 0; i < 16; i++) m[i] = f32(top + i * 4);
    float x0 = f32(node + kNodeRect), y0 = f32(node + kNodeRect + 4);
    float x1 = f32(node + kNodeRect + 8), y1 = f32(node + kNodeRect + 12);
    auto tx = [&](float x, float y) { return m[0] * x + m[4] * y + m[12]; };
    auto ty = [&](float x, float y) { return m[1] * x + m[5] * y + m[13]; };
    Button b;
    b.node = node;
    b.vtable = mem::r32(node);
    float ax = tx(x0, y0), ay = ty(x0, y0), bx = tx(x1, y1), by = ty(x1, y1);
    b.x0 = std::min(ax, bx);
    b.x1 = std::max(ax, bx);
    b.y0 = std::min(ay, by);
    b.y1 = std::max(ay, by);
    b.cx0 = b.x0, b.cy0 = b.y0, b.cx1 = b.x1, b.cy1 = b.y1;
    u32 clip_end = mem::r32(renderer + kRendererClipEnd);
    if (clip_end >= mem::HEAP_BASE + 0x1c && mem::r8(clip_end - 4)) {
        float x = f32(clip_end - 0x1c), y = f32(clip_end - 0x18), w = f32(clip_end - 0x14), h = f32(clip_end - 0x10);
        b.cx0 = std::max(b.x0, x);
        b.cy0 = std::max(b.y0, y);
        b.cx1 = std::min(b.x1, x + w);
        b.cy1 = std::min(b.y1, y + h);
        b.clip_x0 = x, b.clip_y0 = y, b.clip_x1 = x + w, b.clip_y1 = y + h;
        if (b.cx1 < b.cx0) b.cx1 = b.cx0;
        if (b.cy1 < b.cy0) b.cy1 = b.cy0;
    }
    // Accumulated colour (renderer colour stack, pushed by PreRender): a
    // button drawn fully transparent is not really on screen.
    u32 cbeg = mem::r32(renderer + kRendererColorBegin), cend = mem::r32(renderer + kRendererColorBegin + 4);
    b.alpha = cend > cbeg && cend - cbeg < 0x10000 ? mem::r32(cend - 4) >> 24 : 255;
    u32 n = node, root = node;
    for (int depth = 0; n && depth < 64; depth++) {
        root = n;
        n = mem::r32(n + kNodeParent);
    }
    b.root = root;
    b.scroll = scroll_ancestor(node);
    if (b.scroll && b.clip_x1 <= b.clip_x0) b.clip_x0 = 0, b.clip_y0 = 0, b.clip_x1 = 1e9f, b.clip_y1 = 1e9f;
    b.order = g_order++;
    auto it = g_cls_of_vtable.find(b.vtable);
    b.cls = it != g_cls_of_vtable.end() ? it->second : "?";
    g_rec.push_back(b);
}

}  // namespace

std::string class_name(u32 obj) { return rtti::class_name(g_mod, obj); }

namespace {
enum class HookKind { kPost, kPre };

// Wrap virtual `slot` of every class derived from `cls`. The host function
// must not call guest code.
int hook_virtual(const char* cls, u32 slot, HookKind kind, u32 host_stub) {
    std::map<u32, u32> tramp_for_orig;
    int n = 0;
    for (const rtti::VTable& v : rtti::derived_vtables(g_mod, cls)) {
        u32 vt = v.vtable;
        // Every entry up to the slot must be a function: otherwise the class
        // has fewer virtuals (or pure ones) and the slot is not what we want.
        bool ok = true;
        for (u32 i = 0; i <= slot && ok; i++) ok = is_code(mem::r32(vt + i * 4));
        if (!ok) continue;
        u32 at = vt + slot * 4;
        u32 orig = mem::r32(at);
        auto it = tramp_for_orig.find(orig);
        u32 tramp = it != tramp_for_orig.end()
                        ? it->second
                        : (tramp_for_orig[orig] = kind == HookKind::kPost ? hook::post(orig, host_stub)
                                                                          : hook::pre(orig, host_stub));
        mem::w32(at, tramp);
        g_cls_of_vtable[vt] = v.name;
        n++;
    }
    return n;
}

// The game's input-state object (returned by libapp+0x3f7c88, a function-local
// static). Byte +0x132 is set while driving: the game then maps controller
// keys to driving actions (MogaKeyCode's second argument).
u32 g_input_state = 0, g_input_state_guard = 0;
constexpr u32 kGetterOff = 0x3f7c88;

void find_input_state() {
    u32 f = g_mod->base + kGetterOff;
    // ...  f+0x08: ldr r0,[pc,#imm] -> guard address;  f+0x20: ldr r0,[pc,#imm]; f+0x24: add r0,pc,r0
    u32 i0 = mem::r32(f + 0x08), i1 = mem::r32(f + 0x0c);
    u32 j0 = mem::r32(f + 0x20), j1 = mem::r32(f + 0x24);
    if (mem::r32(f) != 0xE92D4830 || (i0 & 0xFFFFF000) != 0xE59F0000 || i1 != 0xE08F0000 ||
        (j0 & 0xFFFFF000) != 0xE59F0000 || j1 != 0xE08F0000) {
        LOGW("controller UI: game state object not found (unexpected game build)");
        return;
    }
    g_input_state_guard = f + 0x0c + 8 + mem::r32(f + 0x08 + 8 + (i0 & 0xFFF));
    g_input_state = f + 0x24 + 8 + mem::r32(f + 0x20 + 8 + (j0 & 0xFFF));
}

// The flow manager decides which screen gets input (FlowManager::OnEvent):
// UILayer+0x1c is the FlowManager; FlowManager+0x24 the current screen (a
// popup when one is open); while FlowManager+0x100 is set or the float at
// +0x104 is positive a transition runs and gets the input instead.
// Screen+0x120 is the root widget of the screen's layout. We learn the
// UILayer when it renders.
u32 g_ui_layer = 0;
constexpr u32 kSlotLayerRender = 14;

// Screen key handler (Screen vtable slot 18): diagnostics for now.
constexpr u32 kSlotScreenOnKey = 18;
}  // namespace

bool init() {
    g_mod = elf::find("libapp.so");
    if (!g_mod) return false;
    const char* hm = std::getenv("NFSMW_UI_HOOKS");
    // 1 = buttons, 2 = screen key log (debugging), 4 = UI layer
    unsigned mask = hm ? (unsigned)std::atoi(hm) : 5u;
    u32 on_prerender = thunks::add("ui_on_prerender", [](cpu::GuestThread& t) {
        auto& R = t.regs();
        if (R[0] & 0xff) record(R[1], R[2]);  // drawn: note where
    });
    u32 on_screen_key = thunks::add("ui_on_screen_key", [](cpu::GuestThread& t) {
        auto& R = t.regs();
        u32 r = R[0], self = R[1], ev = R[2];
        u32 id = mem::r32(self + 4);
        LOGD("screen key: %s %08X id=%s type=%u key=0x%X -> %u%s", class_name(self).c_str(), self,
             id >= mem::NULL_GUARD_END ? mem::str(id) : "?", mem::r32(ev + 4), mem::r32(ev + 8), r & 0xff,
             g_delivering_key ? " (sync)" : " (later)");
    });
    u32 on_layer_render = thunks::add("ui_on_layer_render", [](cpu::GuestThread& t) { g_ui_layer = t.regs()[0]; });
    int buttons = !(mask & 1) ? 0
                              : hook_virtual("N2im11scene2d_new7layouts6ButtonE", kSlotPreRender, HookKind::kPost,
                                             on_prerender);
    int screens = !(mask & 2) ? 0
                              : hook_virtual("N2im3app4flow7screens6ScreenE", kSlotScreenOnKey, HookKind::kPost,
                                             on_screen_key);
    if (mask & 4) hook_virtual("N2im3app4flow7UILayerE", kSlotLayerRender, HookKind::kPre, on_layer_render);
    find_input_state();
    g_active = buttons > 0;
    LOGI("controller UI: %d button classes hooked, %d screen classes", buttons, screens);
    if (!g_active) LOGW("controller UI: button classes not found; menu navigation disabled");
    return g_active;
}

bool active() { return g_active; }

u32 flow_manager() {
    if (!g_ui_layer) return 0;
    u32 fm = mem::r32(g_ui_layer + 0x1c);
    return fm >= mem::HEAP_BASE ? fm : 0;
}

u32 front_screen() {
    u32 fm = flow_manager();
    return fm ? mem::r32(fm + 0x24) : 0;
}

u32 front_root() {
    u32 sc = front_screen();
    return sc >= mem::HEAP_BASE ? mem::r32(sc + 0x120) : 0;
}

const char* front_screen_id() {
    u32 sc = front_screen();
    if (sc < mem::HEAP_BASE) return "";
    u32 id = mem::r32(sc + 4);
    return id >= mem::NULL_GUARD_END && id < mem::g_size ? mem::str(id) : "";
}

// Screens where the game's own controller support (with its own highlight)
// is good, so the D-pad and A go straight to the game there.
// Side panels that open over a screen (the Most Wanted list): while one is
// open, its "_minimise_btn" (the dark area that closes it) is interactive.
// Returns the panel node, or 0.
u32 g_overlay = 0;
bool g_overlay_valid = false;
u32 overlay_panel() {
    if (g_overlay_valid) return g_overlay;
    g_overlay_valid = true;
    g_overlay = 0;
    for (const Button& b : g_last)
        if (node_name(b.node) == "_minimise_btn" && is_interactive(b)) {
            g_overlay = mem::r32(b.node + kNodeParent);
            break;
        }
    return g_overlay;
}

bool has_ancestor(u32 node, u32 anc) {
    for (u32 n = mem::r32(node + kNodeParent), d = 0; n >= mem::HEAP_BASE && d < 32; d++, n = mem::r32(n + kNodeParent))
        if (n == anc) return true;
    return false;
}

bool front_is_native() {
    if (overlay_panel()) return false;  // the game's own navigation stays underneath
    // by screen id
    static const char* const kIds[] = {
        "Profile", "GarageGrid", "StoreScreen",
    };
    // by screen class
    static const char* const kClasses[] = {
        "N2im3app4flow7screens16GarageGridScreenE",
        "N2im3app4flow3nfs11StoreScreenE",
    };
    u32 sc = front_screen();
    if (!sc) return false;
    const char* id = front_screen_id();
    for (const char* n : kIds)
        if (std::strcmp(id, n) == 0) return true;
    std::string cls = class_name(sc);
    for (const char* c : kClasses)
        if (cls == c) return true;
    return false;
}

bool in_transition() {
    u32 fm = flow_manager();
    if (!fm) return false;
    return mem::r8(fm + 0x100) != 0 || f32(fm + 0x104) > 0;
}

bool is_racing() {
    if (!g_input_state || !(mem::r8(g_input_state_guard) & 1)) return false;
    return mem::r8(g_input_state + 0x132) != 0;
}

void frame_begin() {
    g_overlay_valid = false;
    g_last.swap(g_rec);
    g_rec.clear();
    g_order = 0;
}

const std::vector<Button>& visible() { return g_last; }

bool is_interactive(const Button& b) {
    u32 vt = mem::r32(b.node);
    if (vt != b.vtable) return false;  // object gone or replaced
    auto& t = cpu::current();
    u32 inter = mem::r32(vt + kSlotIsInteractive * 4);
    if (!((u32)t.call(inter, {b.node}) & 0xff)) return false;
    float w = f32(b.node + kNodeRect + 8) - f32(b.node + kNodeRect);
    float h = f32(b.node + kNodeRect + 12) - f32(b.node + kNodeRect + 4);
    u32 hit = mem::r32(vt + kSlotHitTest * 4);
    return ((u32)t.call(hit, {b.node, (u32)(int)(w / 2), (u32)(int)(h / 2)}) & 0xff) != 0;
}

void dump(const char* why) {
    u32 sc = front_screen();
    LOGI("controller UI: ui layer %08X, front screen %08X %s id=%s root %08X, transition=%d native=%d", g_ui_layer,
         sc, class_name(sc).c_str(), front_screen_id(), front_root(), in_transition(), front_is_native());
    if (g_ui_layer) {
        u32 fm = mem::r32(g_ui_layer + 0x1c);
        LOGI("  flow manager %08X (%s)", fm, class_name(fm).c_str());
        for (u32 o = 0; o < 0x40 && fm; o += 4) {
            u32 w = mem::r32(fm + o);
            LOGI("    +%02X %08X %s", o, w, w >= mem::HEAP_BASE && w < mem::g_size - 4 ? class_name(w).c_str() : "");
        }
        LOGI("    +100 %02X  +104 %08X", mem::r8(fm + 0x100), mem::r32(fm + 0x104));
    }
    if (u32 fnode = focused_node()) {
        std::string chain;
        u32 n = mem::r32(fnode + kNodeParent);
        for (int d = 0; n >= mem::HEAP_BASE && d < 12; d++) {
            chain += " <- " + class_name(n);
            n = mem::r32(n + kNodeParent);
        }
        LOGI("  focused %08X %s%s", fnode, class_name(fnode).c_str(), chain.c_str());
    }
    LOGI("controller UI (%s): %zu buttons drawn, racing=%d (state %08X: %02X %02X %02X)", why, g_last.size(),
         is_racing(), g_input_state, g_input_state ? mem::r8(g_input_state + 0x132) : 0,
         g_input_state ? mem::r8(g_input_state + 0x133) : 0, g_input_state ? mem::r8(g_input_state + 0x134) : 0);
    for (auto& b : g_last) {
        LOGI("  #%d %08X %-28s %-22s/%-22s vis [%4.0f %4.0f - %4.0f %4.0f] a=%3u%s", b.order, b.node,
             b.cls + 10, node_name(b.node).c_str(), node_name(mem::r32(b.node + kNodeParent)).c_str(), b.cx0, b.cy0,
             b.cx1, b.cy1, b.alpha, is_interactive(b) ? " interactive" : "");
    }
}

// ---- focus navigation --------------------------------------------------------
namespace {
struct Focus {
    u32 node = 0;
    u32 screen = 0;     // front screen when it was chosen
    Button last;        // where it was last seen
    int missing = 0;    // frames it has not been drawn
} g_focus;
bool g_show = false;
int g_anim = 0;

// The highlight as drawn: it glides from button to button instead of jumping,
// fades in when it appears and flashes when A is pressed.
using SteadyClock = std::chrono::steady_clock;
double now_s() { return std::chrono::duration<double>(SteadyClock::now().time_since_epoch()).count(); }
struct Shown {
    bool valid = false;        // drawn on the previous frame
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    double last = 0;           // time of the previous frame
    double since = 0;          // when it appeared
} g_shown;
u32 g_settled_node = 0;    // button the highlight is on (for the light sweep)
double g_settled_at = 0;
struct Flash {
    double at = -1;            // when A was pressed
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
} g_flash;
// After A pressed a button: where it was, so the button that appears in the
// same place on the next screen ("continue", "OK") gets the highlight.
struct LastPress {
    bool pending = false;
    Button rect;
    int frames = 0;
} g_press;
int g_screen_w = 1280, g_screen_h = 720;

float cx(const Button& b) { return (b.cx0 + b.cx1) / 2; }
float cy(const Button& b) { return (b.cy0 + b.cy1) / 2; }

const Button* find_visible(u32 node) {
    for (auto& b : g_last)
        if (b.node == node) return &b;
    return nullptr;
}

// Buttons the player could tap now, de-duplicated (a node drawn twice keeps
// its last position).
// Only the screen that receives input counts (so an open popup is modal);
// if none of the drawn buttons belongs to it, all of them are used.
std::vector<Button> candidates() {
    std::vector<Button> all, front;
    std::set<u32> seen;
    u32 root = front_root();
    u32 panel = overlay_panel();
    for (auto it = g_last.rbegin(); it != g_last.rend(); ++it) {
        const Button& b = *it;
        if (!seen.insert(b.node).second) continue;
        if (b.x1 - b.x0 < 4 || b.y1 - b.y0 < 4 || b.alpha < 16) continue;
        if (panel && !has_ancestor(b.node, panel)) continue;
        if (node_name(b.node) == "_block_input") continue;  // an input blocker, not a button
        // at least half of it must be visible (inside its scroll area and the screen)
        float vx0 = std::max(b.cx0, 0.f), vy0 = std::max(b.cy0, 0.f);
        float vx1 = std::min(b.cx1, (float)g_screen_w), vy1 = std::min(b.cy1, (float)g_screen_h);
        float vis = std::max(0.f, vx1 - vx0) * std::max(0.f, vy1 - vy0);
        if (vis < 0.5f * (b.x1 - b.x0) * (b.y1 - b.y0)) continue;
        if (!is_interactive(b)) continue;
        all.push_back(b);
        if (root && b.root == root) front.push_back(b);
    }
    std::vector<Button>& out = front.empty() ? all : front;
    std::reverse(out.begin(), out.end());
    return out;
}

// How good a first choice a button is, from its layout name: "no"/"cancel"
// in a question (safe), then continue/next/OK-like buttons; back, close and
// restart last.
int name_rank(std::string n) {
    for (char& ch : n) ch = (char)std::tolower((unsigned char)ch);
    auto has = [&](const char* w) { return n.find(w) != std::string::npos; };
    // (checked first: "restart" contains "start")
    if (has("back") || has("close") || has("restart") || has("exit") || has("quit") || has("minimi") ||
        has("maximi") || has("nag") || has("block") || has("freelook") || has("dropdown"))
        return -1;
    if (has("cancel") || n == "btn_no" || has("_no_")) return 4;
    if (has("continue") || has("next") || has("accept") || has("confirm") || has("resume") || has("btn_ok") ||
        has("main_ok") || has("start"))
        return 3;
    if (has("yes")) return 2;
    return 0;
}

// A sensible first button: by name (above), then the one closest to the
// screen's centre, preferring larger buttons.
const Button* pick_start(const std::vector<Button>& c) {
    const Button* best = nullptr;
    float best_score = 1e30f;
    float mx = 0, my = 0;
    for (auto& b : c) {
        mx = std::max(mx, b.x1);
        my = std::max(my, b.y1);
    }
    for (auto& b : c) {
        float dx = cx(b) - mx / 2, dy = cy(b) - my / 2;
        float area = (b.x1 - b.x0) * (b.y1 - b.y0);
        float score = dx * dx + dy * dy - area * 2.f;
        // The bottom menu bar is always there; prefer the screen's own buttons.
        if (std::strstr(b.cls, "NavButton")) score += 40000.f * (mx / 1280.f) * (mx / 1280.f);
        // Buttons that need EA's servers (web pages, store) never work offline.
        if (std::strstr(b.cls, "TestConnectionButton")) score += 1e9f;
        score -= 1e6f * name_rank(node_name(b.node));
        if (score < best_score) {
            best_score = score;
            best = &b;
        }
    }
    return best;
}

void set_focus(const Button& b) {
    g_focus.node = b.node;
    g_focus.screen = front_screen();
    g_focus.last = b;
    g_focus.missing = 0;
    g_anim = 0;
}
}  // namespace

bool has_focus() { return g_focus.node != 0; }
size_t candidate_count() { return candidates().size(); }
void clear_focus() { g_focus = Focus{}; }
void set_highlight_visible(bool on) {
    g_show = on;
    if (!on) g_press.pending = false;  // e.g. the touch screen is used
}
bool highlight_visible() { return g_show && has_focus(); }

void pressed_focused() {
    if (g_shown.valid) g_flash = {now_s(), g_shown.x0, g_shown.y0, g_shown.x1, g_shown.y1};
    g_show = false;
    if (const Button* b = find_visible(g_focus.node)) {
        g_press.rect = *b;
        g_press.pending = true;
        g_press.frames = 0;
    }
}

void update() {
    if (!g_press.pending) return;
    int f = ++g_press.frames;
    if (f > 240 || g_show) {
        g_press.pending = false;
        return;
    }
    if (f < 6 || f % 6 != 0 || in_transition()) return;
    const Button& r = g_press.rect;
    for (const Button& b : candidates()) {
        float ix = std::min(b.x1, r.x1) - std::max(b.x0, r.x0), iy = std::min(b.y1, r.y1) - std::max(b.y0, r.y0);
        if (ix <= 0 || iy <= 0) continue;
        float inter = ix * iy;
        float uni = (b.x1 - b.x0) * (b.y1 - b.y0) + (r.x1 - r.x0) * (r.y1 - r.y0) - inter;
        if (inter / uni > 0.6f) {
            set_focus(b);
            g_show = true;
            g_press.pending = false;
            return;
        }
    }
}

u32 focused_node() { return g_focus.node; }

std::string button_name(u32 node) { return node_name(node); }

bool find_button(const char* name, Button& out) {
    for (const Button& b : g_last)
        if (node_name(b.node) == name && b.alpha >= 16 && is_interactive(b)) {
            out = b;
            return true;
        }
    return false;
}

// Drag request that reveals the next hidden button of a scroll area in
// direction d. scroll_id 0 = the top-most scroll area that has one.
bool scroll_request(Dir d, u32 scroll_id, const Button* from, ScrollRequest* out) {
    const Button* hidden = nullptr;
    float hd = 1e30f;
    int top_order = -1;
    for (const Button& b : g_last) {
        if (!b.scroll || (scroll_id && b.scroll != scroll_id)) continue;
        if (from && b.node == from->node) continue;
        if (u32 panel = overlay_panel(); panel && !has_ancestor(b.node, panel)) continue;
        float vis = std::max(0.f, b.cx1 - b.cx0) * std::max(0.f, b.cy1 - b.cy0);
        if (vis >= 0.5f * (b.x1 - b.x0) * (b.y1 - b.y0)) continue;  // already visible
        // beyond the visible part of its area, in direction d
        float dist;
        switch (d) {
        case Dir::Down: dist = b.y1 - b.clip_y1; break;
        case Dir::Up: dist = b.clip_y0 - b.y0; break;
        case Dir::Right: dist = b.x1 - b.clip_x1; break;
        default: dist = b.clip_x0 - b.x0; break;
        }
        if (dist <= 0) continue;
        if (from) {
            switch (d) {
            case Dir::Down: dist = b.y0 - from->y1; break;
            case Dir::Up: dist = from->y0 - b.y1; break;
            case Dir::Right: dist = b.x0 - from->x1; break;
            default: dist = from->x0 - b.x1; break;
            }
            if (dist < -2) continue;
        } else if (b.order < top_order) {
            continue;  // prefer the area drawn on top
        }
        if (!from && b.order > top_order) {
            top_order = b.order;
            hd = 1e30f;
        }
        if (dist >= hd) continue;
        hd = dist;
        hidden = &b;
    }
    if (!hidden) return false;
    if (out) {
        const Button& h = *hidden;
        float x0 = std::max(h.clip_x0, 0.f), y0 = std::max(h.clip_y0, 0.f);
        float x1 = std::min(h.clip_x1, (float)g_screen_w), y1 = std::min(h.clip_y1, (float)g_screen_h);
        float cw = x1 - x0, ch = y1 - y0;
        ScrollRequest r;
        r.x = from ? cx(*from) : (x0 + x1) / 2;
        r.y = from ? cy(*from) : (y0 + y1) / 2;
        float need;
        switch (d) {
        case Dir::Down: need = h.y1 - y1 + 8; r.dy = -std::clamp(need, 24.f, ch * 0.8f); break;
        case Dir::Up: need = y0 - h.y0 + 8; r.dy = std::clamp(need, 24.f, ch * 0.8f); break;
        case Dir::Right: need = h.x1 - x1 + 8; r.dx = -std::clamp(need, 24.f, cw * 0.8f); break;
        default: need = x0 - h.x0 + 8; r.dx = std::clamp(need, 24.f, cw * 0.8f); break;
        }
        *out = r;
    }
    return true;
}

bool drag_origin(float& x, float& y) {
    // the highlighted button if it is in a list, else the middle of the
    // scroll area on top, else the highlighted button
    const Button* f = has_focus() && g_show ? find_visible(g_focus.node) : nullptr;
    if (f && f->scroll) {
        x = cx(*f);
        y = cy(*f);
        return true;
    }
    const Button* top = nullptr;
    u32 panel = overlay_panel();
    for (const Button& b : g_last)
        if (b.scroll && b.clip_x1 > b.clip_x0 && (!panel || has_ancestor(b.node, panel)) &&
            (!top || b.order > top->order))
            top = &b;
    if (!top) {
        if (!f) return false;
        x = cx(*f);
        y = cy(*f);
        return true;
    }
    float x0 = std::max(top->clip_x0, 0.f), y0 = std::max(top->clip_y0, 0.f);
    float x1 = std::min(top->clip_x1, (float)g_screen_w), y1 = std::min(top->clip_y1, (float)g_screen_h);
    x = (x0 + x1) / 2;
    y = (y0 + y1) / 2;
    return true;
}

bool focused_rect_center(float& x, float& y) {
    if (!has_focus()) return false;
    const Button* b = find_visible(g_focus.node);
    const Button& r = b ? *b : g_focus.last;
    x = cx(r);
    y = cy(r);
    return true;
}

Move move_focus(Dir d, ScrollRequest* scroll) {
    auto c = candidates();
    if (c.empty()) return Move::None;
    const Button* cur = nullptr;
    if (has_focus())
        for (auto& b : c)
            if (b.node == g_focus.node) cur = &b;
    if (!cur) {
        // Nothing highlighted, or that button cannot be used any more (the
        // screen changed after a press): start from the most central button.
        if (const Button* s = pick_start(c)) set_focus(*s);
        return Move::Moved;
    }
    // Spatial search (like Android's FocusFinder): the candidate must lie in
    // the pressed direction; buttons overlapping the current one's row/column
    // win, then the nearest by weighted distance.
    const Button& a = *cur;
    const Button* best = nullptr;
    bool best_beam = false;
    float best_score = 1e30f;
    for (auto& b : c) {
        if (b.node == a.node) continue;
        float major, minor;
        bool beam;
        switch (d) {
        case Dir::Right:
            if (cx(b) <= cx(a) + 1 || b.cx1 <= a.cx1) continue;
            major = std::max(0.f, b.cx0 - a.cx1);
            minor = std::fabs(cy(b) - cy(a));
            beam = b.cy1 > a.cy0 && b.cy0 < a.cy1;
            break;
        case Dir::Left:
            if (cx(b) >= cx(a) - 1 || b.cx0 >= a.cx0) continue;
            major = std::max(0.f, a.cx0 - b.cx1);
            minor = std::fabs(cy(b) - cy(a));
            beam = b.cy1 > a.cy0 && b.cy0 < a.cy1;
            break;
        case Dir::Down:
            if (cy(b) <= cy(a) + 1 || b.cy1 <= a.cy1) continue;
            major = std::max(0.f, b.cy0 - a.cy1);
            minor = std::fabs(cx(b) - cx(a));
            beam = b.cx1 > a.cx0 && b.cx0 < a.cx1;
            break;
        default:  // Up
            if (cy(b) >= cy(a) - 1 || b.cy0 >= a.cy0) continue;
            major = std::max(0.f, a.cy0 - b.cy1);
            minor = std::fabs(cx(b) - cx(a));
            beam = b.cx1 > a.cx0 && b.cx0 < a.cx1;
            break;
        }
        float score = 13 * major * major + minor * minor;
        if ((beam && !best_beam) || (beam == best_beam && score < best_score)) {
            best = &b;
            best_beam = beam;
            best_score = score;
        }
    }
    // A list that continues past its visible edge in this direction is
    // scrolled first, before the highlight leaves it.
    if (a.scroll && (!best || best->scroll != a.scroll) && scroll_request(d, a.scroll, &a, scroll))
        return Move::Scroll;
    // Nothing further that way: scroll the list on top that has more
    // (lists without buttons to highlight, like the Most Wanted list).
    if (!best && scroll_request(d, 0, nullptr, scroll)) return Move::Scroll;
    if (best) set_focus(*best);
    return best ? Move::Moved : Move::None;
}

bool focused_point(float& x, float& y) {
    const Button* cur = has_focus() ? find_visible(g_focus.node) : nullptr;
    if (!cur || !is_interactive(*cur)) {
        auto c = candidates();
        clear_focus();
        if (const Button* s = pick_start(c)) set_focus(*s);
        return false;
    }
    x = cx(*cur);
    y = cy(*cur);
    return true;
}

void draw(int screen_w, int screen_h) {
    g_screen_w = screen_w;
    g_screen_h = screen_h;
    const double now = now_s();
    const float unit = screen_h / 720.f;
    // The short flash where A was pressed (the highlight itself is hidden then).
    const double since_press = now - g_flash.at;
    auto draw_flash = [&] {
        if (g_flash.at < 0 || since_press > 0.22) return;
        overlay::SelectionLook look;
        look.flash = 1.f - (float)(since_press / 0.22);
        look.alpha = look.flash;
        overlay::draw_highlight(screen_w, screen_h, g_flash.x0, g_flash.y0, g_flash.x1, g_flash.y1, look);
    };
    auto hide = [&] {
        g_shown.valid = false;
        draw_flash();
    };
    if (!has_focus()) return hide();
    if (front_screen() != g_focus.screen) {
        clear_focus();  // another screen (or popup) took over
        return hide();
    }
    if (const Button* b = find_visible(g_focus.node)) {
        if (g_show && (g_anim & 7) == 0 && !is_interactive(*b)) {
            clear_focus();  // disabled or hidden by the game (e.g. a panel closed)
            return hide();
        }
        g_focus.last = *b;
        g_focus.missing = 0;
    } else if (++g_focus.missing > 20) {
        clear_focus();  // gone for good (screen changed)
        return hide();
    } else {
        return hide();  // briefly not drawn (transition): keep it but do not show
    }
    if (!g_show) return hide();
    g_anim++;
    const Button& b = g_focus.last;
    if (!g_shown.valid) {
        g_shown = {true, b.cx0, b.cy0, b.cx1, b.cy1, now, now};
        g_settled_node = b.node;
        g_settled_at = now;
    } else {
        // Glide towards the button: about 90 % of the way in 0.1 s, at any frame rate.
        const double dt = std::clamp(now - g_shown.last, 0.0, 0.1);
        const float k = (float)(1.0 - std::exp(-dt / 0.045));
        g_shown.x0 += (b.cx0 - g_shown.x0) * k;
        g_shown.y0 += (b.cy0 - g_shown.y0) * k;
        g_shown.x1 += (b.cx1 - g_shown.x1) * k;
        g_shown.y1 += (b.cy1 - g_shown.y1) * k;
        const float d = std::max({std::fabs(b.cx0 - g_shown.x0), std::fabs(b.cy0 - g_shown.y0),
                                  std::fabs(b.cx1 - g_shown.x1), std::fabs(b.cy1 - g_shown.y1)});
        if (d < 0.5f * unit) g_shown.x0 = b.cx0, g_shown.y0 = b.cy0, g_shown.x1 = b.cx1, g_shown.y1 = b.cy1;
        g_shown.last = now;
        if (b.node != g_settled_node) g_settled_node = b.node, g_settled_at = now;
    }
    overlay::SelectionLook look;
    look.pulse = 0.5f + 0.5f * (float)std::sin((now - g_shown.since) * 2 * 3.14159265 / 1.6);
    look.alpha = (float)std::min(1.0, (now - g_shown.since) / 0.12);
    // A light sweeps across the button 0.35 s after the highlight reaches it,
    // then every 3 s (0.7 s per sweep), so moving through a list stays calm.
    {
        const double t = now - g_settled_at - 0.35;
        if (t >= 0) {
            const double p = std::fmod(t, 3.0);
            if (p < 0.7) look.sheen = (float)(p / 0.7);
        }
    }
    overlay::draw_highlight(screen_w, screen_h, g_shown.x0, g_shown.y0, g_shown.x1, g_shown.y1, look);
}

}  // namespace game::ui
