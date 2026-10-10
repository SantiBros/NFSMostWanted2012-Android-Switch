// Performance settings of the port (game::prefs) put into effect, and frame
// statistics for the debug menu and the frame-rate counter.
#pragma once

#include "core/common.h"

namespace game::perf {

// After the GL context exists: vsync, clocks, 3D resolution.
void init(int screen_w, int screen_h);
// Re-apply the settings that work while playing (after a change in the menu).
void apply();
// Desktop --fps N: frame-rate limit for this run only (not saved).
void set_fps_override(int fps);

// Main loop: around the game tick, and after gl_swap (frame limiter).
void frame_begin();
void record_tick(double tick_ms, double gl_ms, u64 gl_calls);
void frame_end();

struct Stats {
    float fps = 0;        // frames shown per second
    float frame_ms = 0;   // average time between frames
    float tick_ms = 0;    // the game's own work per frame (game code + GL calls)
    float gl_ms = 0;      // of which inside the GL driver
    float gl_calls = 0;   // GL calls per frame
    float worst_ms = 0;   // slowest frame in the last second
    float gl_busy = 0;    // threaded GL: share of time the GL thread was busy (0..1)
    float gl_wait_ms = 0; // threaded GL: time per frame the game thread waited for it
    float swap_ms = 0;    // threaded GL: time per frame in the buffer swap (GPU finishing, display)
    bool gl_thread = false;
};
const Stats& stats();

// The game's detail tier as chosen at start-up, and its name ("Highest", ...).
int startup_tier();
const char* tier_name(int tier);
// GameActivityMain.getPerformanceScore(), which the game turns into its tier.
float performance_score(int screen_w, int screen_h);

// Clocks in Hz the settings ask for right now (0 = stock), after resolving
// "auto" for the current handheld/docked mode.
void wanted_clocks(u32& cpu, u32& gpu, u32& mem);

// Frame-rate counter over the finished frame (when switched on).
void draw_counter(int screen_w, int screen_h);

}  // namespace game::perf
