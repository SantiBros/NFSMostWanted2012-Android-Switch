// Performance settings of the port (game::prefs) put into effect, and frame
// statistics for the debug menu and the frame-rate counter.
#pragma once

#include <vector>

#include "core/common.h"

namespace game::perf {

// After the GL context exists: vsync, clocks, 3D resolution.
void init(int screen_w, int screen_h);
// The real screen after docking / undocking while playing (the game keeps the
// size it started with; see hle::gl_set_output_size).
void set_output_size(int w, int h);
void output_size(int& w, int& h);
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

// Loading (0.3.1). The game shows a loading screen between "Lock: Is being
// locked" and "Unlock: ... Lock count: 0" in its log, and starts a thread for
// every load (a car in the garage, a race). While either is going on the CPU
// runs at 1785 MHz when prefs loading_boost is on (official Switch games do the
// same), and each load's time is kept for the debug menu.
void loading_screen(int lock_count);       // any thread
void loading_thread(u32 id, bool started); // a game thread started once the game runs / ended (any thread)
void game_ticked();                        // main thread, each game frame (load threads count from then on)
bool game_running();
bool loading_now();
bool boosting_now();
struct Load {
    bool screen;  // a loading screen (else a background load, e.g. a car in the garage)
    double secs;
    bool boosted;
};
// The latest loads, newest first (at most 6).
std::vector<Load> recent_loads();
// Frames shown so far.
long frames_shown();
// The clocks the hardware ran at in the last half second (false: not known).
bool clocks_now(u32& cpu, u32& gpu, u32& mem);

// Frame-rate counter over the finished frame (when switched on).
void draw_counter(int screen_w, int screen_h);

}  // namespace game::perf
