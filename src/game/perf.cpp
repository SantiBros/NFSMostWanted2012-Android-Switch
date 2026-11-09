#include "game/perf.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "game/audio.h"
#include "game/overlay.h"
#include "game/prefs.h"
#include "hle/file_cache.h"
#include "hle/gl_queue.h"
#include "hle/hle.h"
#include "hle/jni/jni.h"
#include "core/cpu.h"
#include "core/memory.h"
#include "core/jit_cache.h"
#include "dynarmic/interface/compile_stats.h"
#include "platform/thread.h"
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
std::atomic<long> g_frames{0};

// Loading: the game's loading-screen lock and its loading threads (set from
// any thread), and the boost the main thread puts into effect.
std::atomic<int> g_lock_count{0};
std::atomic<bool> g_game_running{false};
std::mutex g_load_mutex;
struct LoadThread {
    u32 id;
    Clock::time_point since;
};
std::vector<LoadThread> g_load_threads;     // game threads running now (g_load_mutex)
constexpr double kMaxLoadThreadSecs = 180;  // a thread running longer is not a load (a service thread)
std::vector<Load> g_loads;                  // newest first
Clock::time_point g_screen_since, g_thread_since;
bool g_boosting = false;                    // main thread
bool g_boost_capped = false;                // boosted too long: wait for the loads to end
Clock::time_point g_boost_since;
Clock::time_point g_last_loading;
constexpr double kMaxBoostSecs = 120;       // safety net: never boost longer than this in one go

void add_load(bool screen, double secs) {
    std::lock_guard lk(g_load_mutex);
    g_loads.insert(g_loads.begin(), Load{screen, secs, g_boosting});
    if (g_loads.size() > 6) g_loads.resize(6);
}

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
    // The game's own heap (its malloc, shared with the Java objects the port
    // makes for it) and those objects: both should level off, not keep growing.
    // "taken" also counts what the allocator holds for the game's small
    // blocks; "largest free" is the biggest single block still available.
    char heap[160];
    const jni::GcStats gc = jni::gc_stats();
    const mem::HeapStats hs = mem::heap_stats();
    std::snprintf(heap, sizeof heap,
                  "game heap %.0f MiB in use, %.0f of %.0f MiB taken, largest free %.0f MiB, %llu Java objects (%.0f MiB)",
                  hs.in_use / 1048576.0, hs.taken / 1048576.0, hs.size / 1048576.0, hs.largest_free / 1048576.0,
                  (unsigned long long)gc.objects, gc.data_bytes / 1048576.0);
    u64 tex_count = 0, tex_bytes = 0;
    hle::texture_stats(tex_count, tex_bytes);
    char textures[64];
    std::snprintf(textures, sizeof textures, "GL textures %llu (%.0f MiB)", (unsigned long long)tex_count,
                  tex_bytes / 1048576.0);
    // Threads not yet released (the Switch allows an application 96).
    const std::string threads = "threads " + std::to_string(platform::live_threads());
    std::string extra;
    for (const std::string& part : {std::string(jit), jit_cache::summary(), hle::filecache::summary(), audio::summary(), std::string(memory), std::string(heap), std::string(textures), threads})
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

void apply_clocks(bool quiet = false) {
    platform::Clocks c;
    wanted_clocks(c.cpu, c.gpu, c.mem);
    platform::set_clocks(c, quiet);
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
    if (g_boosting) cpu = std::max(cpu, 1785000000u);
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
    hle::gl_set_anisotropy(prefs::get().anisotropy);
    hle::gl_set_msaa(prefs::get().msaa);
}

void loading_screen(int lock_count) {
    const int before = g_lock_count.exchange(lock_count);
    if (before == 0 && lock_count > 0) {
        std::lock_guard lk(g_load_mutex);
        g_screen_since = Clock::now();
    } else if (before > 0 && lock_count == 0) {
        Clock::time_point t0;
        {
            std::lock_guard lk(g_load_mutex);
            t0 = g_screen_since;
        }
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        LOGI("loading screen: %.1f s%s", secs, g_boosting ? " (CPU boost)" : "");
        add_load(true, secs);
    }
}

void game_ticked() { g_game_running.store(true, std::memory_order_relaxed); }
bool game_running() { return g_game_running.load(std::memory_order_relaxed); }

void loading_thread(u32 id, bool started) {
    const auto now = Clock::now();
    double secs = -1;
    {
        std::lock_guard lk(g_load_mutex);
        if (started) {
            if (g_load_threads.empty()) g_thread_since = now;
            g_load_threads.push_back({id, now});
        } else {
            for (size_t i = 0; i < g_load_threads.size(); i++)
                if (g_load_threads[i].id == id) {
                    g_load_threads.erase(g_load_threads.begin() + (long)i);
                    break;
                }
            if (g_load_threads.empty() && g_lock_count.load() == 0)
                secs = std::chrono::duration<double>(now - g_thread_since).count();
        }
    }
    if (secs >= 0) add_load(false, secs);
}

bool loading_now() {
    if (g_lock_count.load() > 0) return true;
    const auto now = Clock::now();
    std::lock_guard lk(g_load_mutex);
    // A load thread is working (not a service thread waiting for work, as the
    // game's first one does for the whole session).
    for (const LoadThread& t : g_load_threads)
        if (std::chrono::duration<double>(now - t.since).count() < kMaxLoadThreadSecs && !cpu::thread_waiting(t.id))
            return true;
    return false;
}
bool boosting_now() { return g_boosting; }

std::vector<Load> recent_loads() {
    std::lock_guard lk(g_load_mutex);
    return g_loads;
}

long frames_shown() { return g_frames.load(); }
bool clocks_now(u32& cpu, u32& gpu, u32& mem) {
    cpu = g_clocks_now.cpu, gpu = g_clocks_now.gpu, mem = g_clocks_now.mem;
    return g_have_clocks;
}

void frame_begin() {
    bool d = platform::is_docked();  // cached by the system library: cheap
    bool reapply = false;
    if (d != g_docked) {
        g_docked = d;
        reapply = true;  // the "auto" GPU clock depends on the mode
    }
    // CPU boost while the game loads (only when it raises the clock). It stays
    // on for a moment after a load, so that browsing cars in the garage (a load
    // each) does not switch the clock for every car.
    const bool loading_raw = loading_now();
    if (loading_raw) g_last_loading = Clock::now();
    const bool loading =
        loading_raw || (g_boosting && std::chrono::duration<double>(Clock::now() - g_last_loading).count() < 1.5);
    if (!loading) g_boost_capped = false;
    const bool useful = prefs::get().loading_boost && prefs::get().cpu_khz < 1785000;
    bool want = useful && loading && !g_boost_capped;
    if (want && g_boosting &&
        std::chrono::duration<double>(Clock::now() - g_boost_since).count() > kMaxBoostSecs) {
        LOGW("loading boost: still loading after %.0f s; back to the normal clock until it ends", kMaxBoostSecs);
        g_boost_capped = true;
        want = false;
    }
    bool boost_changed = false;
    if (want != g_boosting) {
        g_boosting = want;
        if (want) g_boost_since = Clock::now();
        else
            LOGI("loading boost: CPU 1785 MHz for %.1f s while the game loaded",
                 std::chrono::duration<double>(Clock::now() - g_boost_since).count());
        boost_changed = true;
    }
    if (reapply || boost_changed) apply_clocks(/*quiet=*/!reapply);
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
    g_frames.fetch_add(1, std::memory_order_relaxed);
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
