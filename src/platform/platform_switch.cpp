// Nintendo Switch backend (libnx + Mesa/nouveau EGL).
//
// Run the .nro in "title takeover" mode (hold R while starting a game, then
// open the Homebrew Menu): applet mode only gives homebrew ~440 MB, which is
// not enough for the game.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <malloc.h>
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/crash.h"
#include "core/jit_cache.h"
#include "platform/platform.h"

// ---- crash reports (core/crash.h) ---------------------------------------------
// libnx calls __libnx_exception_handler on the crashed thread, on the stack
// below, for a CPU exception of the port (a bad memory access, an undefined
// instruction...). The report goes into log.txt; when the handler returns,
// libnx ends the program and the system shows its error message (Atmosphere
// also writes its crash report to sd:/atmosphere/crash_reports/).
extern "C" {
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
void __libnx_exception_handler(ThreadExceptionDump* ctx);
extern char _start[];   // the start of the .nro's code: offsets in nfsmw.elf count from here
extern char __end__[];  // the end of its memory
}

namespace {
std::atomic<bool> g_crashed{false};

const char* exception_text(u32 desc) {
    switch (desc) {
    case 0x101: return "bad memory access (data abort)";
    case ThreadExceptionDesc_InstructionAbort: return "jump to a bad address (instruction abort)";
    case ThreadExceptionDesc_MisalignedPC: return "jump to a misaligned address";
    case ThreadExceptionDesc_MisalignedSP: return "misaligned stack";
    case ThreadExceptionDesc_Trap: return "undefined instruction";
    case ThreadExceptionDesc_SError: return "memory system error (SError)";
    case ThreadExceptionDesc_BadSVC: return "bad system call";
    default: return "CPU exception";
    }
}

// Readable memory from addr up to the returned end (0 if none).
u64 readable_until(u64 addr) {
    MemoryInfo mi{};
    u32 pi = 0;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr)) || !(mi.perm & Perm_R) || mi.type == MemType_Unmapped) return 0;
    return mi.addr + mi.size;
}
}  // namespace

void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    if (g_crashed.exchange(true)) return;  // a second crash meanwhile: let the system handle it
    crash::HostState s;
    s.what = exception_text(ctx->error_desc);
    s.pc = ctx->pc.x;
    s.lr = ctx->lr.x;
    s.sp = ctx->sp.x;
    s.has_address = ctx->error_desc == 0x101 || ctx->error_desc == ThreadExceptionDesc_InstructionAbort;
    s.fault_address = ctx->far.x;
    s.esr = ctx->esr;
    s.image_base = (u64)(uintptr_t)_start;
    s.image_end = (u64)(uintptr_t)__end__;
    s.image_name = "nfsmw.elf";
    static u64 gprs[29];
    for (int i = 0; i < 29; i++) gprs[i] = ctx->cpu_gprs[i].x;
    s.gprs = gprs;
    s.ngprs = 29;
    // Return-address guesses: stack words that point into the port's code.
    u64 a = s.sp & ~7ull, end = readable_until(a);
    for (; a + 8 <= end && a < s.sp + 0x2000 && s.nstack < 16; a += 8) {
        const u64 v = *reinterpret_cast<const u64*>(a);
        if (v > s.image_base && v < s.image_end) s.stack[s.nstack++] = v;
    }
    crash::report(s);
}

namespace platform {

namespace {

EGLDisplay g_dpy = EGL_NO_DISPLAY;
EGLSurface g_surf = EGL_NO_SURFACE;
EGLContext g_ctx = EGL_NO_CONTEXT;
EGLConfig g_cfg = nullptr;
PadState g_pad;
int g_w = 1280, g_h = 720;  // size the game renders at (fixed at launch)
bool g_quit = false;

}  // namespace


namespace {
bool g_text_console = false;
}

void text_screen(const std::string& text) {
    if (!g_text_console) {
        consoleInit(nullptr);
        logging::g_to_stderr = false;  // the console shows stderr: keep the log off the screen
        g_text_console = true;
    }
    std::fputs("\x1b[2J\x1b[1;1H", stdout);
    std::fputs(text.c_str(), stdout);
    std::fflush(stdout);
    consoleUpdate(nullptr);
}

void text_screen_close() {
    if (!g_text_console) return;
    consoleExit(nullptr);
    g_text_console = false;
    logging::g_to_stderr = true;
}

bool finish_setup_screen(const std::string& self_nro, bool may_restart) {
    // Start again through the homebrew loader (hbloader: the Homebrew Menu,
    // Sphaira...): the game then gets a fresh process and screen.
    const bool restart = may_restart && !self_nro.empty() && envHasNextLoad();
    std::fputs(restart ? "\n  Press A to start the game.\n"
                       : "\n  Press + to close, then start NFS Most Wanted 2012 Loader again.\n",
               stdout);
    std::fflush(stdout);
    consoleUpdate(nullptr);
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        if ((down & HidNpadButton_Plus) || (restart && (down & HidNpadButton_A))) break;
        consoleUpdate(nullptr);
    }
    if (restart) {
        const std::string args = "\"" + self_nro + "\" --after-setup";
        if (R_FAILED(envSetNextLoad(self_nro.c_str(), args.c_str()))) LOGW("setup: could not ask the loader to restart");
    }
    text_screen_close();
    return true;
}

void install_crash_handler() {}  // __libnx_exception_handler above is all it takes

void show_fatal(const char* message) {
    ErrorApplicationConfig c;
    if (R_SUCCEEDED(errorApplicationCreate(&c, "NFS Most Wanted 2012 loader stopped.", message))) {
        errorApplicationShow(&c);
    }
}

void startup_checks() {
    if (appletGetAppletType() != AppletType_Application) {
        // Applet (album) mode only gets ~440 MB: not enough for the game.
        consoleInit(nullptr);
        std::printf("\n  NFS Most Wanted 2012 loader\n\n"
                    "  Please start the Homebrew Menu in title-takeover mode:\n"
                    "  hold R while launching any game, then open this app.\n\n"
                    "  (Applet mode does not have enough memory for the game.)\n\n"
                    "  Press + to exit.\n");
        consoleUpdate(nullptr);
        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            consoleUpdate(nullptr);
        }
        consoleExit(nullptr);
        std::exit(0);
    }
}

GuestSpace reserve_guest_space() {
    GuestSpace gs;
    // The game was written for 1-2 GB phones; 1.25 GiB of guest space leaves
    // room for JIT code caches, Mesa and the loader in title-takeover mode.
    u64 sizes[] = {1280ull << 20, 1024ull << 20, 768ull << 20};
    for (u64 s : sizes) {
        gs.base = aligned_alloc(0x1000, s);
        if (gs.base) {
            gs.size = s;
            break;
        }
    }
    if (gs.base) std::memset(gs.base, 0, 0x10000);
    gs.dirty = gs.size;  // heap memory: may hold an earlier program's data (cleared in mem::init)
    gs.fastmem = false;  // no user-mode fault handling on Horizon: use a page table
    // Guest NULL page(s): with direct memory access the JIT reads and writes
    // guest memory without checks, so make a NULL-pointer access crash at once
    // (as it does on the desktop) instead of silently touching this memory.
    if (gs.base && R_FAILED(svcSetMemoryPermission(gs.base, 0x10000, Perm_None)))
        LOGW("could not protect the guest NULL page");
    return gs;
}

void preferred_display_size(int& width, int& height) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {  // docked
        width = 1920;
        height = 1080;
    } else {
        width = 1280;
        height = 720;
    }
}

bool gl_init(int width, int height) {
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_dpy == EGL_NO_DISPLAY || !eglInitialize(g_dpy, nullptr, nullptr)) {
        LOGE("eglInitialize failed: 0x%X", eglGetError());
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                               EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
                               EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(g_dpy, cfg_attr, &cfg, 1, &n) || n == 0) {
        LOGE("no suitable EGL config");
        return false;
    }
    g_cfg = cfg;
    g_w = width;
    g_h = height;
    NWindow* win = nwindowGetDefault();
    nwindowSetDimensions(win, width, height);
    g_surf = eglCreateWindowSurface(g_dpy, cfg, (EGLNativeWindowType)win, nullptr);
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_surf == EGL_NO_SURFACE || g_ctx == EGL_NO_CONTEXT || !eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
        LOGE("EGL surface/context creation failed: 0x%X", eglGetError());
        return false;
    }
    eglSwapInterval(g_dpy, 1);
    LOGI("EGL ready (%s), %dx%d", eglQueryString(g_dpy, EGL_VENDOR), width, height);
    return true;
}

void gl_swap() { eglSwapBuffers(g_dpy, g_surf); }

int gl_set_swap_interval(int interval) {
    EGLint lo = 0, hi = 1;
    eglGetConfigAttrib(g_dpy, g_cfg, EGL_MIN_SWAP_INTERVAL, &lo);
    eglGetConfigAttrib(g_dpy, g_cfg, EGL_MAX_SWAP_INTERVAL, &hi);
    int n = std::clamp(interval, (int)lo, (int)hi);
    if (!eglSwapInterval(g_dpy, n)) {
        LOGW("eglSwapInterval(%d) failed: 0x%X", n, eglGetError());
        return 1;
    }
    LOGI("swap interval %d (asked %d, driver allows %d-%d)", n, interval, lo, hi);
    return n;
}

void* gl_get_proc(const char* name) { return (void*)eglGetProcAddress(name); }

void gl_release_current() { eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); }
bool gl_make_current() { return eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx) == EGL_TRUE; }

void clocks_exit();

// The exit is locked only while the loader runs the console at non-stock
// clocks, so that closing the game from the HOME menu first gives the stock
// clocks back. At stock clocks (the default) it is never locked: the system
// can always close the loader, even if the game hangs.
bool g_exit_locked = false;
void lock_exit_for_clocks() {
    if (g_exit_locked) return;
    if (R_SUCCEEDED(appletLockExit())) g_exit_locked = true;
}
void unlock_exit() {
    if (!g_exit_locked) return;
    g_exit_locked = false;
    appletUnlockExit();
}

bool gl_should_close() {
    if (!appletMainLoop()) {
        // Closed from the HOME menu while the exit is locked (custom clocks):
        // give the console its stock clocks back, then let the system end the
        // process. Returning to the Homebrew loader is not an option while the
        // game's threads are still running.
        LOGI("exit requested: restoring stock clocks and closing");
        clocks_exit();
        jit_cache::save();
        logging::flush();
        std::fflush(nullptr);
        unlock_exit();
        for (;;) svcSleepThread(1000000000ll);
    }
    return g_quit;
}

bool gl_screenshot(const std::string&) { return false; }
std::string debug_command() { return {}; }
std::string take_pending_screenshot() { return {}; }
void sample_set_main_thread() {}
void sample_start() {}
void sample_gate(bool, const char* volatile*) {}

// ---- input ------------------------------------------------------------------
// Motion sensors: each controller style has its own six-axis handle
// (handheld, Pro Controller / full key, the two Joy-Cons of a pair, and a
// single Joy-Con held sideways).
static HidSixAxisSensorHandle g_sixaxis[6];
bool g_sixaxis_started = false;

static void input_init_once() {
    static bool done = false;
    if (done) return;
    done = true;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();
    hidGetSixAxisSensorHandles(&g_sixaxis[0], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    hidGetSixAxisSensorHandles(&g_sixaxis[1], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
    hidGetSixAxisSensorHandles(&g_sixaxis[2], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    hidGetSixAxisSensorHandles(&g_sixaxis[4], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyLeft);
    hidGetSixAxisSensorHandles(&g_sixaxis[5], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyRight);
    for (auto& h : g_sixaxis) hidStartSixAxisSensor(h);
    g_sixaxis_started = true;
}

// Gravity from the six-axis sensor of whichever controller is in use.
static void read_motion(InputState& out) {
    HidSixAxisSensorState st{};
    size_t n = 0;
    u32 styles = padGetStyleSet(&g_pad);
    if (styles & HidNpadStyleTag_NpadHandheld) {
        n = hidGetSixAxisSensorStates(g_sixaxis[0], &st, 1);
    } else if (styles & HidNpadStyleTag_NpadFullKey) {
        n = hidGetSixAxisSensorStates(g_sixaxis[1], &st, 1);
    } else if (styles & HidNpadStyleTag_NpadJoyDual) {
        // Joy-Con pair held in the grip: the right one, else the left one.
        u32 attr = padGetAttributes(&g_pad);
        if (attr & HidNpadAttribute_IsRightConnected) n = hidGetSixAxisSensorStates(g_sixaxis[3], &st, 1);
        else if (attr & HidNpadAttribute_IsLeftConnected) n = hidGetSixAxisSensorStates(g_sixaxis[2], &st, 1);
    } else if (styles & HidNpadStyleTag_NpadJoyLeft) {
        n = hidGetSixAxisSensorStates(g_sixaxis[4], &st, 1);
    } else if (styles & HidNpadStyleTag_NpadJoyRight) {
        n = hidGetSixAxisSensorStates(g_sixaxis[5], &st, 1);
    }
    if (n == 0 || !(st.attributes & HidSixAxisSensorAttribute_IsConnected)) return;
    out.motion = true;
    out.gx = st.acceleration.x;
    out.gy = st.acceleration.y;
    out.gz = st.acceleration.z;
}

bool poll_input(InputState& out) {
    input_init_once();
    padUpdate(&g_pad);
    u64 held = padGetButtons(&g_pad);
    out = InputState{};
    struct {
        u64 hid;
        unsigned ours;
    } map[] = {
        {HidNpadButton_A, BTN_A},       {HidNpadButton_B, BTN_B},         {HidNpadButton_X, BTN_X},
        {HidNpadButton_Y, BTN_Y},       {HidNpadButton_L, BTN_L},         {HidNpadButton_R, BTN_R},
        {HidNpadButton_ZL, BTN_ZL},     {HidNpadButton_ZR, BTN_ZR},       {HidNpadButton_Plus, BTN_PLUS},
        {HidNpadButton_Minus, BTN_MINUS}, {HidNpadButton_Up, BTN_UP},     {HidNpadButton_Down, BTN_DOWN},
        {HidNpadButton_Left, BTN_LEFT}, {HidNpadButton_Right, BTN_RIGHT},
    };
    for (auto& m : map)
        if (held & m.hid) out.buttons |= m.ours;
    HidAnalogStickState l = padGetStickPos(&g_pad, 0);
    auto norm = [](s32 v) {
        float f = (float)v / (float)JOYSTICK_MAX;
        if (f > -0.08f && f < 0.08f) f = 0;  // dead zone
        return std::clamp(f, -1.f, 1.f);
    };
    out.lx = norm(l.x);
    out.ly = -norm(l.y);  // Android: +Y is down
    HidAnalogStickState r = padGetStickPos(&g_pad, 1);
    out.rx = norm(r.x);
    out.ry = -norm(r.y);
    read_motion(out);
    out.lt = (held & HidNpadButton_ZL) ? 1.f : 0.f;
    out.rt = (held & HidNpadButton_ZR) ? 1.f : 0.f;

    HidTouchScreenState ts{};
    if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
        out.touch = true;
        // The panel reports 1280x720 coordinates; the game may be rendering at
        // 1920x1080 if it was started docked and then taken out of the dock.
        out.touch_x = (float)ts.touches[0].x * g_w / 1280.f;
        out.touch_y = (float)ts.touches[0].y * g_h / 720.f;
    }
    return !g_quit;
}

// Sound output: audio_switch.cpp.

// ---- clocks ---------------------------------------------------------------------
// CPU / GPU / memory clocks through clkrst (8.0.0+) or pcv (older), the same
// services sys-clk uses. Only clock rates that Nintendo's own performance
// profiles use are offered by the debug menu.
namespace {
enum { kCpu, kGpu, kMem, kModules };
const PcvModule kPcvModule[kModules] = {PcvModule_CpuBus, PcvModule_GPU, PcvModule_EMC};
const PcvModuleId kPcvModuleId[kModules] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};

// Stock clocks of the system's performance configurations (from sys-clk's
// apm_profile_table.c): id, CPU, GPU, memory in Hz.
struct ApmConfig {
    u32 id, hz[kModules];
};
const ApmConfig kApmConfigs[] = {
    {0x00010000, {1020000000, 384000000, 1600000000}}, {0x00010001, {1020000000, 768000000, 1600000000}},
    {0x00010002, {1224000000, 691200000, 1600000000}}, {0x00020000, {1020000000, 230400000, 1600000000}},
    {0x00020001, {1020000000, 307200000, 1600000000}}, {0x00020002, {1224000000, 230400000, 1600000000}},
    {0x00020003, {1020000000, 307200000, 1331200000}}, {0x00020004, {1020000000, 384000000, 1331200000}},
    {0x00020005, {1020000000, 307200000, 1065600000}}, {0x00020006, {1020000000, 384000000, 1065600000}},
    {0x92220007, {1020000000, 460800000, 1600000000}}, {0x92220008, {1020000000, 460800000, 1331200000}},
    {0x92220009, {1785000000, 76800000, 1600000000}},  {0x9222000A, {1785000000, 76800000, 1331200000}},
    {0x9222000B, {1020000000, 76800000, 1600000000}},  {0x9222000C, {1020000000, 76800000, 1331200000}},
};

// Fallback stock clocks per mode (handheld, docked) when the system's
// current profile cannot be read: the default application profiles.
const u32 kFallbackStock[2][kModules] = {{1020000000, 307200000, 1331200000},
                                          {1020000000, 768000000, 1600000000}};

std::recursive_mutex g_clk_mutex;  // main thread vs. fatal() on another thread
bool g_clk_tried = false, g_clk_ok = false, g_use_clkrst = false;
ClkrstSession g_clk[kModules];
Service g_apm_sys;
bool g_apm_sys_ok = false;
u32 g_target[kModules] = {0, 0, 0};    // 0 = stock
bool g_managed[kModules] = {false, false, false};  // we changed this clock
u32 g_settled[kModules] = {0, 0, 0};   // rate the hardware picked after our last change
bool g_background = false;             // HOME menu / sleep: stock clocks
bool g_exiting = false;                // closing: stock clocks, no more changes
bool g_force = false;                  // re-apply now
u64 g_next_check = 0;
AppletHookCookie g_hook;

bool clk_init() {
    if (g_clk_tried) return g_clk_ok;
    g_clk_tried = true;
    if (hosversionAtLeast(8, 0, 0)) {
        g_clk_ok = R_SUCCEEDED(clkrstInitialize());
        if (g_clk_ok) {
            g_use_clkrst = true;
            for (int m = 0; m < kModules; m++)
                if (R_FAILED(clkrstOpenSession(&g_clk[m], kPcvModuleId[m], 3))) g_clk_ok = false;
        }
    } else {
        g_clk_ok = R_SUCCEEDED(pcvInitialize());
    }
    g_apm_sys_ok = R_SUCCEEDED(smGetService(&g_apm_sys, "apm:sys"));
    if (!g_clk_ok) LOGW("clocks: clkrst/pcv not available; clock settings are ignored");
    return g_clk_ok;
}

u32 get_hz(int m) {
    u32 hz = 0;
    if (g_use_clkrst) clkrstGetClockRate(&g_clk[m], &hz);
    else pcvGetClockRate(kPcvModule[m], &hz);
    return hz;
}
void set_hz(int m, u32 hz) {
    Result rc = g_use_clkrst ? clkrstSetClockRate(&g_clk[m], hz) : pcvSetClockRate(kPcvModule[m], hz);
    if (R_FAILED(rc)) LOGW("clocks: setting module %d to %u Hz failed (0x%X)", m, hz, rc);
    g_settled[m] = get_hz(m);  // the hardware may round to a rate it supports
}

// The clock the system's current performance profile wants.
u32 stock_hz(int m) {
    if (g_apm_sys_ok && hosversionAtLeast(9, 0, 0)) {
        u32 conf = 0;
        if (R_SUCCEEDED(serviceDispatchOut(&g_apm_sys, 7, conf)))  // GetCurrentPerformanceConfiguration
            for (const ApmConfig& c : kApmConfigs)
                if (c.id == conf) return c.hz[m];
    }
    return kFallbackStock[is_docked() ? 1 : 0][m];
}

void apply_clocks() {
    std::lock_guard lk(g_clk_mutex);
    if (!clk_init()) return;
    for (int m = 0; m < kModules; m++) {
        bool want_override = g_target[m] != 0 && !g_background && !g_exiting;
        u32 want = 0;
        if (want_override) want = g_target[m];
        else if (g_managed[m]) want = stock_hz(m);  // give the stock clock back once
        if (!want) continue;
        u32 cur = get_hz(m);
        // Already there, or at the rate the hardware chose for our request.
        bool ok = cur == want || (g_managed[m] && want_override && cur == g_settled[m]);
        if (!ok) set_hz(m, want);
        g_managed[m] = want_override;
    }
}

void applet_hook(AppletHookType type, void*) {
    switch (type) {
    case AppletHookType_OnFocusState: {
        bool bg = appletGetFocusState() != AppletFocusState_InFocus;
        if (bg != g_background) {
            g_background = bg;
            LOGI("clocks: game %s, %s clocks", bg ? "in the background" : "back in front", bg ? "stock" : "chosen");
            apply_clocks();
        }
        break;
    }
    case AppletHookType_OnOperationMode:
    case AppletHookType_OnPerformanceMode:
    case AppletHookType_OnResume:
        g_force = true;  // the system has just set its own clocks
        break;
    default:
        break;
    }
}
}  // namespace

bool clocks_supported() {
    std::lock_guard lk(g_clk_mutex);
    return clk_init();
}

void set_clocks(const Clocks& c) {
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        appletHook(&g_hook, applet_hook, nullptr);
    }
    if (c.cpu || c.gpu || c.mem) lock_exit_for_clocks();
    {
        std::lock_guard lk(g_clk_mutex);
        g_target[kCpu] = c.cpu;
        g_target[kGpu] = c.gpu;
        g_target[kMem] = c.mem;
    }
    apply_clocks();
    auto mhz = [](u32 hz) {
        char b[24];
        if (!hz) return std::string("stock");
        std::snprintf(b, sizeof b, "%.1f MHz", hz / 1e6);
        return std::string(b);
    };
    LOGI("clocks: cpu %s, gpu %s, mem %s", mhz(c.cpu).c_str(), mhz(c.gpu).c_str(), mhz(c.mem).c_str());
}

bool get_clocks(Clocks& c) {
    std::lock_guard lk(g_clk_mutex);
    if (!clk_init()) return false;
    c.cpu = get_hz(kCpu);
    c.gpu = get_hz(kGpu);
    c.mem = get_hz(kMem);
    return true;
}

bool is_docked() { return appletGetOperationMode() == AppletOperationMode_Console; }

std::string system_info() {
    char buf[384];
    const u32 v = hosversionGet();
    const char* how = "an unknown way";
    switch (appletGetAppletType()) {
    case AppletType_Application: how = "as an application (R + a game, or a forwarder)"; break;
    case AppletType_SystemApplication: how = "as a system application"; break;
    case AppletType_LibraryApplet: how = "as an applet (the Album)"; break;
    case AppletType_OverlayApplet: how = "as an overlay"; break;
    case AppletType_SystemApplet: how = "as the system applet"; break;
    default: break;
    }
    // The loader that started the .nro ("nx-hbloader ...", or another menu's).
    std::string loader;
    if (const char* li = envGetLoaderInfo()) {
        loader.assign(li, strnlen(li, std::min<u64>(envGetLoaderInfoSize(), 64)));
        if (const size_t nl = loader.find('\n'); nl != std::string::npos) loader.resize(nl);
    }
    u64 used = 0, total = 0;
    memory_usage(used, total);
    std::snprintf(buf, sizeof buf, "system: firmware %u.%u.%u%s, started %s%s%s, memory %llu MiB free of %llu MiB",
                  HOSVER_MAJOR(v), HOSVER_MINOR(v), HOSVER_MICRO(v), hosversionIsAtmosphere() ? " (Atmosphere)" : "",
                  how, loader.empty() ? "" : " by ", loader.c_str(), (unsigned long long)((total - used) >> 20),
                  (unsigned long long)(total >> 20));
    return buf;
}

bool memory_usage(u64& used, u64& total) {
    return R_SUCCEEDED(svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0)) &&
           R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0));
}

void tick() {
    // The system re-applies its own clocks after docking, sleep and other
    // events; check twice a second and put ours back.
    u64 now = armTicksToNs(armGetSystemTick()) / 1000000;
    if (!g_force && now < g_next_check) return;
    g_next_check = now + 500;
    g_force = false;
    bool any = g_managed[kCpu] || g_managed[kGpu] || g_managed[kMem] || g_target[kCpu] || g_target[kGpu] ||
               g_target[kMem];
    if (any) apply_clocks();
}

// Leave the console at its stock clocks (exit, fatal error).
void clocks_exit() {
    std::lock_guard lk(g_clk_mutex);
    if (g_exiting) return;
    g_exiting = true;
    if (g_clk_ok) apply_clocks();
    if (g_clk_tried) {
        if (g_use_clkrst) {
            for (auto& s : g_clk) clkrstCloseSession(&s);
            clkrstExit();
        } else if (g_clk_ok) {
            pcvExit();
        }
        if (g_apm_sys_ok) serviceClose(&g_apm_sys);
    }
}

}  // namespace platform

// ---- process start-up -------------------------------------------------------------
extern "C" void userAppInit(void) {
    romfsInit();
    // No socketInitializeDefault(): online services are blocked, so the
    // console's network service is never even opened.
    // Run the main thread and its JIT on core 0 at a high priority; worker
    // threads created later spread over cores 1 and 2.
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
    // Get a message when the game goes to the HOME menu / sleep (to give the
    // stock clocks back). The exit is only locked once custom clocks are in
    // use (see lock_exit_for_clocks()).
    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);
}

extern "C" void userAppExit(void) {
    platform::clocks_exit();
    platform::unlock_exit();
    if (platform::g_sixaxis_started)
        for (auto& h : platform::g_sixaxis) hidStopSixAxisSensor(h);
    audoutStopAudioOut();
    audoutExit();
    romfsExit();
}
