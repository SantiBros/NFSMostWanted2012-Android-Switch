// Everything that differs between the desktop prototype and the Switch build
// lives behind this interface. The rest of the loader is platform neutral.
#pragma once

#include <string>

#include "core/common.h"

namespace platform {

// The host region that mirrors the guest address space: guest address A is
// at base + A for A < size. With `fastmem` the full 4 GiB is reserved and the
// JIT accesses memory directly (faults are caught); without it (Switch) a
// smaller window is used and the JIT goes through a page table.
struct GuestSpace {
    void* base = nullptr;
    u64 size = 0;
    bool fastmem = false;
};
GuestSpace reserve_guest_space();


// Called first thing in main(); may refuse to continue (e.g. Switch applet mode).
void startup_checks();
// Tell the user about a fatal error in a way they will see (log is written first).
void show_fatal(const char* message);
// Catch the port's own crashes (CPU exceptions / fatal signals) and pass them
// to crash::report (core/crash.h).
void install_crash_handler();

// The first-start setup screen (game/setup.cpp): plain text, redrawn whole on
// each call (Switch: the text console; desktop: printed when it closes).
void text_screen(const std::string& text);
void text_screen_close();
// After the setup screen: start the port again on a clean screen (Switch: the
// homebrew loader loads self_nro again when this process ends). Returns true
// when the caller should end the process now, false to carry on (desktop).
bool finish_setup_screen(const std::string& self_nro, bool may_restart);

// Graphics: create a GLES2-capable context of the given size and make it
// current on the calling thread. Returns false on failure.
bool gl_init(int width, int height);
// The display size that suits the console's current mode. Switch: 1280x720
// handheld, 1920x1080 docked. Desktop: leaves the values unchanged.
void preferred_display_size(int& width, int& height);
void gl_swap();
void* gl_get_proc(const char* name);
// Threaded GL (hle/gl_queue): give up the context on the calling thread, and
// make it current on the calling (GL) thread.
void gl_release_current();
bool gl_make_current();
bool gl_should_close();
// Vertical sync: 0 = show each frame at once, 1 = wait for the next screen
// refresh (60 Hz), 2 = every second refresh (30 fps). Returns the interval
// that now paces gl_swap (0 if the platform does not wait, e.g. desktop).
int gl_set_swap_interval(int interval);

// Console clocks in Hz (Switch). 0 = the console's normal (stock) clock for
// the current mode. set_clocks keeps the requested clocks applied (the system
// resets them on dock/undock and after sleep) and gives the stock clocks back
// while the game is in the background (HOME menu).
struct Clocks {
    u32 cpu = 0, gpu = 0, mem = 0;
};
bool clocks_supported();
void set_clocks(const Clocks& target);
bool get_clocks(Clocks& current);  // what the hardware runs at now
bool is_docked();
// Once per frame on the main thread (re-applies clocks when needed).
void tick();

// Desktop debugging: a pending "cmd <word>" from the control channel ("" if none).
std::string debug_command();
// Desktop debugging: sampling profiler of the main thread (debug command "sample").
void sample_set_main_thread();
void sample_start();
// Samples are only taken while the gate is open and *in_host is null.
void sample_gate(bool on, const char* volatile* in_host);

// Capture the current back buffer to a PNG (desktop debugging aid). Must run
// on the thread that has the GL context.
bool gl_screenshot(const std::string& path);
// Desktop: a screenshot the control channel asked for ("" if none); the main
// loop takes it once the frame is complete.
std::string take_pending_screenshot();

// Input polling. Returns false when the user asked to quit.
struct InputState {
    float lx = 0, ly = 0;       // left stick, -1..1 (+y = down)
    float rx = 0, ry = 0;       // right stick
    float lt = 0, rt = 0;       // triggers 0..1
    unsigned buttons = 0;       // see BTN_* below
    bool touch = false;
    float touch_x = 0, touch_y = 0;  // pixels
    // Motion sensor (Switch six-axis): direction of gravity in the
    // controller's frame, in G. Axes as the Switch reports them: x to the
    // right, y up the screen (away from the player when held flat), z out of
    // the screen/face. Held upright and still: (0, -1, 0).
    bool motion = false;
    float gx = 0, gy = 0, gz = 0;
};
enum : unsigned {
    BTN_A = 1u << 0, BTN_B = 1u << 1, BTN_X = 1u << 2, BTN_Y = 1u << 3,
    BTN_L = 1u << 4, BTN_R = 1u << 5, BTN_ZL = 1u << 6, BTN_ZR = 1u << 7,
    BTN_PLUS = 1u << 8, BTN_MINUS = 1u << 9,
    BTN_UP = 1u << 10, BTN_DOWN = 1u << 11, BTN_LEFT = 1u << 12, BTN_RIGHT = 1u << 13,
};
bool poll_input(InputState& out);

// Audio: interleaved signed 16-bit PCM. audio_write blocks while the output
// queue is full, which paces the caller in real time. audio_pause tells the
// output that the mixer has stopped for a while (an empty queue is then not a
// dropout). Switch: audio_switch.cpp.
bool audio_open(int rate, int channels, int buffer_frames);
void audio_write(const s16* samples, int frames);
void audio_pause();
struct AudioStats {
    u32 dropouts = 0;  // times the output ran dry since the start
    int queue_ms = 0;  // sound queued ahead of the speakers
};
AudioStats audio_stats();

// Memory the process uses, and (Switch) how much it may use, in bytes. False
// when not known.
bool memory_usage(u64& used, u64& total);
// One line for log.txt about the system the port runs on: firmware, how it
// was started, the memory it may use ("" if nothing to say).
std::string system_info();

}  // namespace platform
