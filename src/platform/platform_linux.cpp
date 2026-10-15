// Desktop Linux backend: headless EGL (Mesa) rendering to an off-screen
// surface, with PNG screenshots so the output can be inspected.
#include <sys/mman.h>
#if !defined(NFSMW_NO_GL)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "external/stb/stb_image_write.h"
#include "platform/platform.h"

namespace platform {

GuestSpace reserve_guest_space() {
    GuestSpace gs;
#if defined(NFSMW_SWITCH_MEMORY_MODEL)
    // Mimic the Switch build: a smaller window and no fastmem.
    gs.size = 1536ull << 20;
    gs.base = mmap(nullptr, gs.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    gs.fastmem = false;
#else
    const size_t size = (4ull << 30) + 0x10000;
    gs.base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (gs.base != MAP_FAILED) mprotect(gs.base, 0x10000, PROT_NONE);  // guest NULL page(s)
    gs.size = 4ull << 30;
    gs.fastmem = true;
#endif
    if (gs.base == MAP_FAILED) gs.base = nullptr;
    return gs;
}

void startup_checks() {}
void show_fatal(const char*) {}

namespace {
std::string g_last_screen;
}
void text_screen(const std::string& text) { g_last_screen = text; }
void text_screen_close() {
    if (g_last_screen.empty()) return;
    std::fputs(g_last_screen.c_str(), stdout);
    std::fflush(stdout);
    g_last_screen.clear();
}
bool finish_setup_screen(const std::string&, bool) {
    text_screen_close();
    return false;  // the desktop build carries on in the same process
}

#if defined(NFSMW_NO_GL)
// Smoke-test build without a GPU (used for the ARM64 cross build under
// qemu): every GL entry point is a no-op returning 0.
extern "C" u64 nfsmw_gl_noop() { return 0; }
bool gl_init(int, int) { return true; }
void preferred_display_size(int&, int&) {}
void gl_swap() {}
void* gl_get_proc(const char*) { return (void*)&nfsmw_gl_noop; }
bool gl_should_close() { return false; }
bool gl_screenshot(const std::string&) { return false; }
int gl_set_swap_interval(int) { return 0; }
void gl_release_current() {}
bool gl_make_current() { return true; }
#else
namespace {
EGLDisplay g_dpy = EGL_NO_DISPLAY;
EGLSurface g_surf = EGL_NO_SURFACE;
EGLContext g_ctx = EGL_NO_CONTEXT;
int g_w = 0, g_h = 0;
}  // namespace

bool gl_init(int width, int height) {
    g_w = width;
    g_h = height;
    auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (getPlatformDisplay) g_dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (g_dpy == EGL_NO_DISPLAY) g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor;
    if (!eglInitialize(g_dpy, &major, &minor)) {
        LOGE("eglInitialize failed: 0x%X", eglGetError());
        return false;
    }
    LOGI("EGL %d.%d (%s)", major, minor, eglQueryString(g_dpy, EGL_VENDOR));
    const EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                               EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                               EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(g_dpy, cfg_attr, &cfg, 1, &n) || n == 0) {
        LOGE("no suitable EGL config");
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint pb_attr[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    g_surf = eglCreatePbufferSurface(g_dpy, cfg, pb_attr);
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_surf == EGL_NO_SURFACE || g_ctx == EGL_NO_CONTEXT || !eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
        LOGE("EGL surface/context creation failed: 0x%X", eglGetError());
        return false;
    }
    LOGI("GL renderer: %s | %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    return true;
}

void preferred_display_size(int&, int&) {}

// An off-screen surface never blocks on swap, so the software rasterizer would
// fall seconds behind the game. Wait for the frame like a real display does.
void gl_swap() {
    eglSwapBuffers(g_dpy, g_surf);
    glFinish();
}

void* gl_get_proc(const char* name) { return (void*)eglGetProcAddress(name); }

void gl_release_current() { eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); }
bool gl_make_current() { return eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx) == EGL_TRUE; }

bool gl_should_close() { return false; }

// The off-screen surface has no display to wait for: frame pacing is done by
// the loader's frame limiter instead.
int gl_set_swap_interval(int) { return 0; }

bool gl_screenshot(const std::string& path) {
    std::vector<u8> px((size_t)g_w * g_h * 4);
    static const int mode = std::getenv("NFSMW_SHOT_MODE") ? std::atoi(std::getenv("NFSMW_SHOT_MODE")) : 0;
    GLint fb = 0;
    if (mode == 0 || mode == 2) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    auto t0 = std::chrono::steady_clock::now();
    glFinish();
    auto t1 = std::chrono::steady_clock::now();
    if (mode == 0 || mode == 3) glReadPixels(0, 0, g_w, g_h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    auto t2 = std::chrono::steady_clock::now();
    LOGD("screenshot: finish %.1f ms, readpixels %.1f ms",
         std::chrono::duration<double, std::milli>(t1 - t0).count(),
         std::chrono::duration<double, std::milli>(t2 - t1).count());
    if (mode == 0 || mode == 2) glBindFramebuffer(GL_FRAMEBUFFER, fb);
    // flip vertically, drop alpha
    std::vector<u8> rgb((size_t)g_w * g_h * 3);
    for (int y = 0; y < g_h; y++)
        for (int x = 0; x < g_w; x++)
            for (int c = 0; c < 3; c++)
                rgb[((size_t)y * g_w + x) * 3 + c] = px[((size_t)(g_h - 1 - y) * g_w + x) * 4 + c];
    return stbi_write_png(path.c_str(), g_w, g_h, 3, rgb.data(), g_w * 3) != 0;
}
#endif  // NFSMW_NO_GL

// Scripted input for headless testing. NFSMW_INPUT is a ';'-separated list:
//   "<frame> tap <x> <y>"             touch for 4 frames
//   "<frame> press <BTN> [frames]"    hold a button (A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT)
//   "<frame> stick <x> <y> <frames>"  left stick, -1..1
//   "<frame> rstick <x> <y> <frames>" right stick
// Frames count calls to poll_input (one per game frame).
//
// NFSMW_CONTROL=<dir> drives the game interactively instead: lines appended to
// <dir>/cmd.txt run one after another ("press A [frames]", "tap x y",
// "stick x y frames", "rstick x y frames", "wait frames", "shot name" ->
// <dir>/name.png). <dir>/status shows the current frame and queue length.
namespace {
struct ScriptEvent {
    int frame, duration;
    enum { Tap, Press, Stick, RStick, Shot, Wait, Cmd, Tilt } kind;
    float x, y;
    unsigned button;
    std::string name;
};
std::vector<ScriptEvent> g_script;
int g_poll_count = 0;
unsigned button_from_name(const std::string& n) {
    static const std::pair<const char*, unsigned> names[] = {
        {"A", BTN_A}, {"B", BTN_B}, {"X", BTN_X}, {"Y", BTN_Y}, {"L", BTN_L}, {"R", BTN_R}, {"ZL", BTN_ZL},
        {"ZR", BTN_ZR}, {"PLUS", BTN_PLUS}, {"MINUS", BTN_MINUS}, {"UP", BTN_UP}, {"DOWN", BTN_DOWN},
        {"LEFT", BTN_LEFT}, {"RIGHT", BTN_RIGHT}};
    // "ZL+ZR+MINUS": several buttons held together
    unsigned all = 0;
    size_t i = 0;
    while (i <= n.size()) {
        size_t j = n.find('+', i);
        if (j == std::string::npos) j = n.size();
        std::string part = n.substr(i, j - i);
        for (auto& [k, v] : names)
            if (part == k) all |= v;
        i = j + 1;
    }
    return all;
}
// Parses "<kind> args..." (without the frame number).
bool parse_event(const char* item, ScriptEvent& e) {
    char kind[16] = {}, arg[64] = {};
    float a = 0, b = 0;
    int d = 0;
    if (std::sscanf(item, "%15s", kind) != 1) return false;
    if (!std::strcmp(kind, "tap") && std::sscanf(item, "%*s %f %f", &a, &b) == 2) {
        e.kind = ScriptEvent::Tap; e.x = a; e.y = b; e.duration = 4;
    } else if (!std::strcmp(kind, "press") && std::sscanf(item, "%*s %15s %d", arg, &d) >= 1) {
        e.kind = ScriptEvent::Press; e.button = button_from_name(arg); e.duration = d > 0 ? d : 4;
    } else if ((!std::strcmp(kind, "stick") || !std::strcmp(kind, "rstick")) &&
               std::sscanf(item, "%*s %f %f %d", &a, &b, &d) == 3) {
        e.kind = kind[0] == 'r' ? ScriptEvent::RStick : ScriptEvent::Stick; e.x = a; e.y = b; e.duration = d;
    } else if (!std::strcmp(kind, "tilt") && std::sscanf(item, "%*s %f", &a) == 1) {
        // "tilt <degrees>": the console held upright and turned like a
        // steering wheel (positive = clockwise = right); stays until changed.
        e.kind = ScriptEvent::Tilt; e.x = a; e.duration = 1;
    } else if (!std::strcmp(kind, "wait") && std::sscanf(item, "%*s %d", &d) == 1) {
        e.kind = ScriptEvent::Wait; e.duration = d;
    } else if (!std::strcmp(kind, "shot") && std::sscanf(item, "%*s %63s", arg) == 1) {
        e.kind = ScriptEvent::Shot; e.name = arg; e.duration = 1;
    } else if (!std::strcmp(kind, "cmd") && std::sscanf(item, "%*s %63s", arg) == 1) {
        e.kind = ScriptEvent::Cmd; e.name = arg; e.duration = 1;
    } else {
        return false;
    }
    return true;
}
void load_script() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    const char* s = std::getenv("NFSMW_INPUT");
    if (!s) return;
    std::string all = s;
    size_t i = 0;
    while (i < all.size()) {
        size_t j = all.find(';', i);
        if (j == std::string::npos) j = all.size();
        std::string item = all.substr(i, j - i);
        ScriptEvent e{};
        int consumed = 0;
        if (std::sscanf(item.c_str(), "%d %n", &e.frame, &consumed) == 1 && parse_event(item.c_str() + consumed, e))
            g_script.push_back(e);
        i = j + 1;
    }
    LOGI("input script: %zu events", g_script.size());
}

// Interactive control (NFSMW_CONTROL).
std::string g_ctl_dir;
std::string g_debug_cmd;
std::string g_pending_shot;
float g_tilt_deg = 0;  // scripted motion sensor (desktop testing)
long g_ctl_offset = 0;
std::vector<ScriptEvent> g_queue;
int g_queue_next_frame = 0;
int g_ctl_lines = 0;  // command lines read so far  // frame at which the next queued command may start
void control_poll(int f) {
    static bool init = false;
    if (!init) {
        init = true;
        if (const char* d = std::getenv("NFSMW_CONTROL")) g_ctl_dir = d;
    }
    if (g_ctl_dir.empty()) return;
    if (FILE* fp = std::fopen((g_ctl_dir + "/cmd.txt").c_str(), "rb")) {
        std::fseek(fp, g_ctl_offset, SEEK_SET);
        char line[256];
        while (std::fgets(line, sizeof line, fp)) {
            if (!std::strchr(line, '\n')) break;  // incomplete line: read it next time
            g_ctl_offset = std::ftell(fp);
            g_ctl_lines++;
            ScriptEvent e{};
            if (parse_event(line, e)) g_queue.push_back(e);
            else if (line[0] != '\n' && line[0] != '#') LOGW("control: cannot parse '%s'", line);
        }
        std::fclose(fp);
    }
    // Start queued commands one after another (2 idle frames between inputs).
    while (!g_queue.empty() && f >= g_queue_next_frame) {
        ScriptEvent e = g_queue.front();
        g_queue.erase(g_queue.begin());
        e.frame = f;
        if (e.kind == ScriptEvent::Cmd) {
            g_debug_cmd = e.name;
            continue;
        }
        if (e.kind == ScriptEvent::Tilt) {
            g_tilt_deg = e.x;
            LOGI("control: tilt %.1f degrees", e.x);
            continue;
        }
        if (e.kind == ScriptEvent::Shot) {
            // Taken when this frame is complete; later commands start next frame.
            g_pending_shot = g_ctl_dir + "/" + e.name + ".png";
            g_queue_next_frame = f + 1;
            break;
        }
        g_queue_next_frame = f + e.duration + (e.kind == ScriptEvent::Wait ? 0 : 2);
        if (e.kind != ScriptEvent::Wait) g_script.push_back(e);
    }
    if (f % 10 == 0 || g_queue.empty()) {
        if (FILE* st = std::fopen((g_ctl_dir + "/status").c_str(), "wb")) {
            std::fprintf(st, "frame %d lines %d queue %zu %s\n", f, g_ctl_lines, g_queue.size(),
                         g_queue.empty() && f >= g_queue_next_frame && g_pending_shot.empty() ? "idle" : "busy");
            std::fclose(st);
        }
    }
}
}  // namespace

std::string debug_command() {
    std::string c;
    c.swap(g_debug_cmd);
    return c;
}

std::string take_pending_screenshot() {
    std::string p;
    p.swap(g_pending_shot);
    return p;
}

// No clock control on the desktop (the settings are kept, not applied).
bool clocks_supported() { return false; }
void set_clocks(const Clocks&) {}
bool get_clocks(Clocks&) { return false; }
bool is_docked() { return false; }
void tick() {}

bool poll_input(InputState& out) {
    load_script();
    out = InputState{};
    int f = ++g_poll_count;
    control_poll(f);
    for (auto& e : g_script) {
        if (f < e.frame || f >= e.frame + e.duration) continue;
        if (f == e.frame && g_ctl_dir.empty()) LOGI("input script: event at poll %d", f);
        switch (e.kind) {
        case ScriptEvent::Tap: out.touch = true; out.touch_x = e.x; out.touch_y = e.y; break;
        case ScriptEvent::Press: out.buttons |= e.button; break;
        case ScriptEvent::Stick: out.lx = e.x; out.ly = e.y; break;
        case ScriptEvent::RStick: out.rx = e.x; out.ry = e.y; break;
        default: break;
        }
    }
    if (!g_ctl_dir.empty() || std::getenv("NFSMW_FAKE_MOTION")) {
        // Console upright, facing the player, rotated clockwise by the angle:
        // gravity = (sin a, -cos a, 0) in the Switch frame.
        float r = g_tilt_deg * 3.14159265f / 180.f;
        out.motion = true;
        out.gx = std::sin(r);
        out.gy = -std::cos(r);
        out.gz = 0;
    }
    return true;
}

namespace {
FILE* g_wav = nullptr;
int g_rate = 0;
u32 g_wav_bytes = 0;
std::chrono::steady_clock::time_point g_audio_clock;
void wav_header(FILE* f, int rate, u32 data_bytes) {
    auto w32 = [f](u32 v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [f](u16 v) { std::fwrite(&v, 2, 1, f); };
    std::fseek(f, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, f); w32(36 + data_bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16); w16(1); w16(2); w32(rate); w32(rate * 4); w16(4); w16(16);
    std::fwrite("data", 1, 4, f); w32(data_bytes);
}
}  // namespace

bool audio_open(int rate, int channels, int) {
    if (channels != 2) return false;
    g_rate = rate;
    g_audio_clock = std::chrono::steady_clock::now();
    if (const char* p = std::getenv("NFSMW_WAV")) {
        g_wav = std::fopen(p, "wb");
        if (g_wav) wav_header(g_wav, rate, 0);
    }
    return true;
}

void audio_write(const s16* samples, int frames) {
    if (g_wav) {
        std::fwrite(samples, 4, frames, g_wav);
        g_wav_bytes += frames * 4;
        long pos = std::ftell(g_wav);
        wav_header(g_wav, g_rate, g_wav_bytes);
        std::fseek(g_wav, pos, SEEK_SET);
        std::fflush(g_wav);
    }
    // pace like a real device
    g_audio_clock += std::chrono::microseconds((long long)frames * 1000000 / g_rate);
    const auto now = std::chrono::steady_clock::now();
    if (g_audio_clock < now - std::chrono::milliseconds(200)) g_audio_clock = now;  // after a pause: no catching up
    std::this_thread::sleep_until(g_audio_clock);
}

void audio_pause() {}

bool memory_usage(u64& used, u64& total) {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return false;
    unsigned long size = 0, resident = 0;
    const bool ok = std::fscanf(f, "%lu %lu", &size, &resident) == 2;
    std::fclose(f);
    used = (u64)resident * 4096;
    total = 0;
    return ok;
}
AudioStats audio_stats() { return {}; }

}  // namespace platform

// ---- sampling profiler (desktop debugging) ------------------------------------
// Debug command "sample": for 3 seconds, interrupt the main thread 2000 times
// a second and record where it was (host instruction pointer). Written to
// $NFSMW_SAMPLE_OUT with a copy of /proc/self/maps; with PERF_BUILDID_DIR set,
// Dynarmic writes perf-<pid>.map, which maps JIT code back to guest addresses
// (tools/profile.py does the analysis).
#if defined(__x86_64__)
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>

#include <atomic>
namespace platform {
namespace {
pthread_t g_sample_main;
constexpr int kMaxSamples = 8000;
unsigned long g_samples[kMaxSamples];
std::atomic<int> g_nsamples{0};
std::atomic<bool> g_sampling{false};

std::atomic<bool> g_gate{false};
const char* volatile* g_in_host = nullptr;
void on_sample(int, siginfo_t*, void* ctx) {
    // Only translated game code: inside the game tick, not inside a host call.
    if (!g_gate.load(std::memory_order_relaxed) || (g_in_host && *g_in_host)) return;
    int i = g_nsamples.load(std::memory_order_relaxed);
    if (i >= kMaxSamples) return;
    g_samples[i] = (unsigned long)static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_RIP];
    g_nsamples.store(i + 1, std::memory_order_relaxed);
}
}  // namespace

void sample_set_main_thread() { g_sample_main = pthread_self(); }
void sample_gate(bool on, const char* volatile* in_host) {
    g_in_host = in_host;
    g_gate.store(on, std::memory_order_relaxed);
}

void sample_start() {
    const char* out = std::getenv("NFSMW_SAMPLE_OUT");
    if (!out || g_sampling.exchange(true)) return;
    struct sigaction sa {};
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
    g_nsamples = 0;
    std::thread([path = std::string(out)] {
        for (int i = 0; i < 40000 && g_nsamples.load() < kMaxSamples; i++) {
            pthread_kill(g_sample_main, SIGPROF);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        FILE* f = std::fopen(path.c_str(), "w");
        if (f) {
            int n = g_nsamples.load();
            std::fprintf(f, "# samples %d\n", n);
            for (int i = 0; i < n; i++) std::fprintf(f, "%lx\n", g_samples[i]);
            std::fprintf(f, "# maps\n");
            if (FILE* m = std::fopen("/proc/self/maps", "r")) {
                char line[512];
                while (std::fgets(line, sizeof line, m)) std::fputs(line, f);
                std::fclose(m);
            }
            std::fclose(f);
            LOGI("sampler: %d samples written to %s", n, path.c_str());
        }
        g_sampling = false;
    }).detach();
}
}  // namespace platform
#else
namespace platform {
void sample_set_main_thread() {}
void sample_gate(bool, const char* volatile*) {}
void sample_start() {}
}  // namespace platform
#endif

// ---- crash reports (core/crash.h) ---------------------------------------------
// SIGSEGV is also how Dynarmic's fastmem works on the desktop: its handler is
// asked first, and only a fault it does not handle (it then restores the
// default action) is a crash.
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>

#include "core/crash.h"

extern "C" char __executable_start[];
extern "C" char _end[];

namespace platform {
namespace {
constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
struct sigaction g_prev[32];
std::atomic<bool> g_crashed{false};

const char* signal_text(int sig) {
    switch (sig) {
    case SIGSEGV: return "bad memory access (SIGSEGV)";
    case SIGBUS: return "bad memory access (SIGBUS)";
    case SIGILL: return "illegal instruction (SIGILL)";
    case SIGFPE: return "arithmetic error (SIGFPE)";
    default: return "fatal signal";
    }
}

void on_crash_signal(int sig, siginfo_t* si, void* raw) {
    struct sigaction& prev = g_prev[sig];
    if ((prev.sa_flags & SA_SIGINFO) && prev.sa_sigaction) {
        prev.sa_sigaction(sig, si, raw);
        struct sigaction now {};
        sigaction(sig, nullptr, &now);
        if (now.sa_handler != SIG_DFL) return;  // handled (a fastmem access)
    } else if (prev.sa_handler == SIG_IGN) {
        return;
    }
    if (!g_crashed.exchange(true)) {
        crash::HostState s;
        s.what = signal_text(sig);
        s.has_address = sig == SIGSEGV || sig == SIGBUS;
        s.fault_address = (u64)(uintptr_t)si->si_addr;
        s.image_base = (u64)(uintptr_t)__executable_start;
        s.image_end = (u64)(uintptr_t)_end;
        s.image_name = "nfsmw";
        auto* uc = static_cast<ucontext_t*>(raw);
#if defined(__x86_64__)
        s.pc = (u64)uc->uc_mcontext.gregs[REG_RIP];
        s.sp = (u64)uc->uc_mcontext.gregs[REG_RSP];
#elif defined(__aarch64__)
        s.pc = uc->uc_mcontext.pc;
        s.sp = uc->uc_mcontext.sp;
        s.lr = uc->uc_mcontext.regs[30];
#endif
        crash::report(s);
    }
    // Let the fault happen again with the default action (core dump).
    signal(sig, SIG_DFL);
}
}  // namespace

void install_crash_handler() {
    static char alt_stack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof alt_stack;
    sigaltstack(&ss, nullptr);
    for (int sig : kSignals) {
        struct sigaction sa {};
        sa.sa_sigaction = on_crash_signal;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, &g_prev[sig]);
    }
}
}  // namespace platform
