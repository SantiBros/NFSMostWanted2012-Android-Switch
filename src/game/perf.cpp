#include "game/perf.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "game/audio.h"
#include "game/overlay.h"
#include "game/prefs.h"
#include "hle/file_cache.h"
#include "hle/gl_queue.h"
#include "hle/hle.h"
#include "core/jit_cache.h"
#include "dynarmic/interface/compile_stats.h"
#include "platform/platform.h"

namespace game::perf {

namespace {
using Clock = std::chrono::steady_clock;

Stats g_stats;
int g_screen_w = 0, g_screen_h = 0;  // the game's screen
int g_out_w = 0, g_out_h = 0;        // the real screen (0: the game's)
int g_paced_interval = 0;   // swap interval that paces gl_swap (0 = none)
bool g_limiter = false;     // sleep to hold the frame-rate limit
double g_period_us = 0;
Clock::time_point g_next_frame;
bool g_have_next = false;
Clock::time_point g_last_end;
bool g_have_last = false;
bool g_docked = false;

// Statistics window (half a second).
Clock::time_point g_win_start;
int g_win_frames = 0, g_win_ticks = 0;
double g_win_tick = 0, g_win_gl = 0, g_win_calls = 0, g_win_worst = 0;

platform::Clocks g_clocks_now;
bool g_have_clocks = false;

// Summary for the log every 10 seconds (log.txt in the port's folder), so a
// console's numbers come back with the log file.
struct LogWindow {
    double secs = 0, frames = 0, tick = 0, gl_busy = 0, gl_wait = 0, swap = 0, calls = 0, worst = 0;
    int windows = 0;
    int hitches = 0;  // frames 1.5x longer than the target (what an average fps hides)
} g_log;
void log_summary() {
    const LogWindow& w = g_log;
    if (w.windows == 0 || w.secs <= 0 || w.frames <= 0) return;
    char clocks[96] = "clocks n/a";
    auto mhz = [](u32 hz) { return (int)std::lround(hz / 1e6); };
    if (g_have_clocks)
        std::snprintf(clocks, sizeof clocks, "cpu %d gpu %d mem %d MHz", mhz(g_clocks_now.cpu), mhz(g_clocks_now.gpu),
                      mhz(g_clocks_now.mem));
    const auto& p = prefs::get();
    const double fps = w.frames / w.secs, frame_ms = w.secs * 1000 / w.frames, game_ms = w.tick / w.windows;
    const double busy = w.gl_busy / w.windows, swap_ms = w.swap / w.windows;
    // What holds the frame rate back, as far as these numbers tell.
    const int limit = p.fps_limit;
    const char* limited_by = "";
    if (limit && fps >= limit - 1.0) limited_by = "on target";
    else if (game_ms > 0.8 * frame_ms) limited_by = "game code (CPU, core 0): a higher CPU clock helps";
    else if (busy > 0.85) limited_by = "GL driver (CPU, core 2): a higher CPU clock helps";
    else if (swap_ms > 0.4 * frame_ms) limited_by = "GPU: a higher GPU clock helps";
    else limited_by = "mixed";
    // New game code translated by the main thread's JIT in this window (each
    // new piece of code costs time the first time it runs).
    static Dynarmic::CompileStats last;
    const Dynarmic::CompileStats& now = Dynarmic::t_compile_stats;  // perf runs on the main thread
    char jit[96] = "";
    if (now.blocks != last.blocks)
        std::snprintf(jit, sizeof jit, "JIT: %llu new blocks, %.0f ms (main thread)",
                      (unsigned long long)(now.blocks - last.blocks), (double)(now.ns - last.ns) / 1e6);
    last = now;
    char memory[64] = "";
    u64 used = 0, total = 0;
    if (platform::memory_usage(used, total)) {
        if (total) std::snprintf(memory, sizeof memory, "memory %llu of %llu MiB", (unsigned long long)(used >> 20), (unsigned long long)(total >> 20));
        else std::snprintf(memory, sizeof memory, "memory %llu MiB", (unsigned long long)(used >> 20));
    }
    std::string extra;
    for (const std::string& part : {std::string(jit), jit_cache::summary(), hle::filecache::summary(), audio::summary(), std::string(memory)})
        if (!part.empty()) extra += " | " + part;
    LOGI("perf: %.1f fps (frame %.1f ms, slowest %.0f ms, %d hitches) | game %.1f ms/frame | GL thread %.0f%% busy, "
         "waited %.1f ms/frame, swap %.1f ms/frame | %.0f GL calls | %s %dx%d, detail %s, limit %s, vsync %s | %s | %s%s",
         fps, frame_ms, w.worst, w.hitches, game_ms, 100 * busy, w.gl_wait / w.windows, swap_ms, w.calls / w.windows,
         g_docked ? "docked" : "handheld", g_out_w ? g_out_w : g_screen_w, g_out_h ? g_out_h : g_screen_h,
         tier_name(startup_tier()),
         limit ? std::to_string(limit).c_str() : "off", p.vsync ? "on" : "off", clocks, limited_by, extra.c_str());
}

// Thresholds of the game's tier choice (libapp.so, function at 0x547B40):
// score > 1.6 Highest, > 1.32 High, > 1.1 Medium, else Low.
int tier_from_score(float s) {
    if (s > 1.6f) return prefs::kQualityHighest;
    if (s > 1.32f) return prefs::kQualityHigh;
    if (s > 1.1f) return prefs::kQualityMedium;
    return prefs::kQualityLow;
}

int g_fps_override = 0;  // desktop --fps, for this run only

void apply_pacing() {
    prefs::Values p = prefs::get();
    if (g_fps_override > 0) p.fps_limit = g_fps_override;
    int want = p.vsync ? (p.fps_limit == 30 ? 2 : 1) : 0;
    hle::glq::sync([&] { g_paced_interval = platform::gl_set_swap_interval(want); });  // on the GL thread
    // Frames the display alone would allow; sleep only when that is above the limit.
    double paced_fps = g_paced_interval > 0 ? 60.0 / g_paced_interval : 1e9;
    g_limiter = p.fps_limit > 0 && paced_fps > p.fps_limit + 0.5;
    g_period_us = p.fps_limit > 0 ? 1e6 / p.fps_limit : 0;
    g_have_next = false;
}

void apply_clocks() {
    platform::Clocks c;
    wanted_clocks(c.cpu, c.gpu, c.mem);
    platform::set_clocks(c);
}

std::string mhz(u32 hz) {
    char b[24];
    if (hz % 1000000) std::snprintf(b, sizeof b, "%.1f", hz / 1e6);
    else std::snprintf(b, sizeof b, "%u", hz / 1000000);
    return b;
}
}  // namespace

const Stats& stats() { return g_stats; }
void set_fps_override(int fps) { g_fps_override = fps; }

float performance_score(int screen_w, int screen_h) {
    // Scores inside the game's bands for each tier (see tier_from_score).
    switch (prefs::at_startup().quality) {
    case prefs::kQualityHigh: return 1.5f;
    case prefs::kQualityMedium: return 1.2f;
    case prefs::kQualityLow: return 1.0f;
    default: break;
    }
    // Highest: GameActivityMain.calcPerformanceScore() for the Switch's CPU (4
    // cores at 1785 MHz) and screen, same arithmetic as the Java code. (It is
    // given the frequency in MHz where phones report GHz, so the result is far
    // above the game's top threshold.)
    float mhz_cpu = 1785.f;
    int cores = 4;
    float cpu = 3.0f * mhz_cpu + mhz_cpu * (float)(std::min(cores, 4) - 1) * 0.5f;
    float screen = (float)(std::sqrt((double)screen_w * screen_h) * 0.001 * 0.5);
    return ((cpu + 1.f) / (screen + 1.f)) * 0.33333334f;
}

int startup_tier() { return tier_from_score(performance_score(g_screen_w ? g_screen_w : 1280, g_screen_h ? g_screen_h : 720)); }

const char* tier_name(int tier) {
    switch (tier) {
    case prefs::kQualityHighest: return "Highest";
    case prefs::kQualityHigh: return "High";
    case prefs::kQualityMedium: return "Medium";
    case prefs::kQualityLow: return "Low";
    default: return "?";
    }
}

void wanted_clocks(u32& cpu, u32& gpu, u32& mem) {
    const auto& p = prefs::get();
    cpu = p.cpu_khz > 0 ? (u32)p.cpu_khz * 1000u : 0;
    if (p.gpu_khz == prefs::kClockAuto) gpu = g_docked ? 0 : 460800000u;  // handheld: Nintendo's top handheld clock
    else gpu = p.gpu_khz > 0 ? (u32)p.gpu_khz * 1000u : 0;
    mem = p.mem_khz > 0 ? (u32)p.mem_khz * 1000u : 0;
}

void set_output_size(int w, int h) {
    g_out_w = w == g_screen_w && h == g_screen_h ? 0 : w;
    g_out_h = w == g_screen_w && h == g_screen_h ? 0 : h;
}
void output_size(int& w, int& h) {
    w = g_out_w ? g_out_w : g_screen_w;
    h = g_out_h ? g_out_h : g_screen_h;
}

void init(int screen_w, int screen_h) {
    g_screen_w = screen_w;
    g_screen_h = screen_h;
    g_docked = platform::is_docked();
    hle::gl_set_screen_size(screen_w, screen_h);
    apply();
    LOGI("game detail tier: %s", tier_name(startup_tier()));
}

void apply() {
    apply_pacing();
    apply_clocks();
    hle::gl_set_scene_scale(prefs::get().scene_scale / 100.f);
}

void frame_begin() {
    bool d = platform::is_docked();  // cached by the system library: cheap
    if (d != g_docked) {
        g_docked = d;
        apply_clocks();  // the "auto" GPU clock depends on the mode
    }
    platform::tick();
}

void record_tick(double tick_ms, double gl_ms, u64 gl_calls) {
    g_win_tick += tick_ms;
    g_win_gl += gl_ms;
    g_win_calls += (double)gl_calls;
    g_win_ticks++;
}

void frame_end() {
    // Frame limiter (when the display's refresh does not already hold the limit).
    if (g_limiter && g_period_us > 0) {
        auto now = Clock::now();
        auto period = std::chrono::microseconds((long long)g_period_us);
        if (!g_have_next || now > g_next_frame + period) {
            g_next_frame = now + period;  // first frame, or fell behind: no catching up
            g_have_next = true;
        } else {
            static const bool busy = std::getenv("NFSMW_BUSY_WAIT") != nullptr;
            if (busy) {
                while (Clock::now() < g_next_frame) {
                }
            } else {
                std::this_thread::sleep_until(g_next_frame);
            }
            g_next_frame += period;
        }
    }

    auto now = Clock::now();
    if (g_have_last) {
        double ms = std::chrono::duration<double, std::milli>(now - g_last_end).count();
        g_win_worst = std::max(g_win_worst, ms);
        const int limit = prefs::get().fps_limit;
        if (ms > 1.5 * 1000.0 / (limit > 0 ? limit : 30)) g_log.hitches++;
    } else {
        g_win_start = now;
    }
    g_last_end = now;
    g_have_last = true;
    g_win_frames++;
    double win_ms = std::chrono::duration<double, std::milli>(now - g_win_start).count();
    if (win_ms >= 500 && g_win_frames > 0) {
        g_stats.fps = (float)(g_win_frames * 1000.0 / win_ms);
        g_stats.frame_ms = (float)(win_ms / g_win_frames);
        if (g_win_ticks) {
            g_stats.tick_ms = (float)(g_win_tick / g_win_ticks);
            g_stats.gl_ms = (float)(g_win_gl / g_win_ticks);
            g_stats.gl_calls = (float)(g_win_calls / g_win_ticks);
        }
        g_stats.worst_ms = (float)g_win_worst;
        g_stats.gl_thread = hle::glq::active();
        if (g_stats.gl_thread) {
            float busy, waited, swap = 0;
            hle::glq::stats(busy, waited, &swap);
            g_stats.gl_busy = busy;
            g_stats.gl_wait_ms = waited / std::max(1, g_win_frames);
            g_stats.swap_ms = swap / std::max(1, g_win_frames);
        }
        g_log.secs += win_ms / 1000;
        g_log.frames += g_win_frames;
        g_log.tick += g_stats.tick_ms;
        g_log.gl_busy += g_stats.gl_busy;
        g_log.gl_wait += g_stats.gl_wait_ms;
        g_log.swap += g_stats.swap_ms;
        g_log.calls += g_stats.gl_calls;
        g_log.worst = std::max(g_log.worst, (double)g_win_worst);
        g_log.windows++;
        if (g_log.secs >= 10) {
            log_summary();
            g_log = LogWindow{};
        }
        g_win_start = now;
        g_win_frames = g_win_ticks = 0;
        g_win_tick = g_win_gl = g_win_calls = g_win_worst = 0;
        g_have_clocks = platform::get_clocks(g_clocks_now);
    }
}

void draw_counter(int screen_w, int screen_h) {
    using namespace overlay;
    const auto& p = prefs::get();
    if (p.overlay == prefs::kOverlayOff) return;
    if (!begin(screen_w, screen_h)) return;
    const float u = screen_h / 720.f;
    const Stats& s = g_stats;
    float target = p.fps_limit > 0 ? (float)p.fps_limit : 60.f;
    Color fps_color = s.fps >= target * 0.95f ? Color{0.45f, 1.f, 0.45f, 1.f}
                      : s.fps >= 20.f         ? Color{1.f, 0.85f, 0.2f, 1.f}
                                              : Color{1.f, 0.4f, 0.35f, 1.f};
    char line[160];
    if (p.overlay == prefs::kOverlayFps) {
        std::snprintf(line, sizeof line, "%.0f FPS", s.fps);
        float w = text_width(line, 18 * u) + 14 * u;
        rect(0, 0, w, 26 * u, {0.f, 0.f, 0.f, 0.55f});
        text(7 * u, 19 * u, 18 * u, line, fps_color);
        end();
        return;
    }
    std::string lines[4];
    std::snprintf(line, sizeof line, "%.1f FPS   frame %.1f ms   slowest %.0f ms", s.fps, s.frame_ms, s.worst_ms);
    lines[0] = line;
    if (s.gl_thread)
        std::snprintf(line, sizeof line, "game %.1f ms   GL thread %.0f%% busy, swap %.1f ms   (%.0f GL calls, waited %.1f ms)",
                      s.tick_ms, s.gl_busy * 100, s.swap_ms, s.gl_calls, s.gl_wait_ms);
    else
        std::snprintf(line, sizeof line, "game %.1f ms   (GL %.1f ms, %.0f calls)", s.tick_ms, s.gl_ms, s.gl_calls);
    lines[1] = line;
    if (g_have_clocks)
        std::snprintf(line, sizeof line, "CPU %s   GPU %s   MEM %s MHz   %s", mhz(g_clocks_now.cpu).c_str(),
                      mhz(g_clocks_now.gpu).c_str(), mhz(g_clocks_now.mem).c_str(), g_docked ? "docked" : "handheld");
    else
        std::snprintf(line, sizeof line, "clocks: not available on this system");
    lines[2] = line;
    float sc = hle::gl_scene_scale();
    int ow, oh;
    output_size(ow, oh);
    std::snprintf(line, sizeof line, "%dx%d   3D %dx%d   detail %s   limit %s%s", ow, oh, (int)std::lround(ow * sc),
                  (int)std::lround(oh * sc), tier_name(startup_tier()),
                  p.fps_limit ? (std::to_string(p.fps_limit) + " FPS").c_str() : "off", p.vsync ? "   VSync" : "");
    lines[3] = line;
    float px = 16 * u, lh = 21 * u, w = 0;
    for (auto& l : lines) w = std::max(w, text_width(l, px));
    rect(0, 0, w + 16 * u, lh * 4 + 10 * u, {0.f, 0.f, 0.f, 0.6f});
    for (int i = 0; i < 4; i++)
        text(8 * u, (i + 1) * lh, px, lines[i], i == 0 ? fps_color : Color{0.92f, 0.92f, 0.92f, 1.f});
    end();
}

}  // namespace game::perf
