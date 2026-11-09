// The port's own options, saved in settings.ini in the port's folder
// (sd:/switch/NFSMostWanted2012/settings.ini on the Switch, see game/paths.h):
//  - gyro steering, switched in the game's Options > Controls > TILT TO STEER;
//  - the performance settings of the debug menu (ZL + ZR + Minus).
// The file is plain text and can also be edited by hand.
#pragma once

#include <string>

namespace game::prefs {

// The game's detail tier ("Tier = ..." in the log). Left alone the game would
// pick Highest on the Switch; Medium is recommended because it needs about a
// fifth less CPU time and a quarter fewer GL calls per frame (measured on the
// desktop build), and the CPU is what limits this port.
enum Quality : int { kQualityHighest = 0, kQualityHigh, kQualityMedium, kQualityLow };
enum Overlay : int { kOverlayOff = 0, kOverlayFps, kOverlayFull };
enum Effects : int { kEffectsGame = 0, kEffectsHigh, kEffectsHighest };
enum Vibration : int { kVibrationOff = 0, kVibrationLow, kVibrationNormal, kVibrationStrong };
// Clock settings, in kHz. Special values:
constexpr int kClockStock = 0;  // whatever the console's own profile uses
constexpr int kClockAuto = -1;  // GPU only: 460.8 MHz handheld, stock docked

struct Values {
    // Gyro steering
    bool gyro_steering = false;
    float gyro_max_angle = 30;  // degrees of tilt for full lock (10-60)
    bool gyro_invert = false;

    // Performance (debug menu)
    int fps_limit = 30;      // 30, 60, or 0 = no limit
    bool vsync = true;
    int scene_scale = 100;   // 3D scene resolution in % of the screen (50-100)
    int quality = kQualityMedium;
    // Switch clocks: stock by default. With the GL thread, the GL call filter
    // and direct memory access the port is meant to run at the console's
    // normal clocks; higher ones are there for anyone who wants more headroom.
    int cpu_khz = kClockStock;
    int gpu_khz = kClockStock;
    int mem_khz = kClockStock;
    // While the game loads (loading screens, a car in the garage), the CPU runs
    // at 1785 MHz, as official Switch games do, and goes back to cpu_khz after.
    bool loading_boost = true;
    // Graphics (0.3.1): anisotropic texture filtering (1 = off, 2-16) and
    // anti-aliasing of the picture (0 = off, 2 or 4 samples). Both change live.
    int anisotropy = 4;
    int msaa = 0;
    // The effects the game gives fast phones (its feature flags of detail
    // level High, or Highest), added to detail level Medium without High's
    // extra objects: kEffectsGame = as the detail level has them.
    int effects = kEffectsHigh;
    int overlay = kOverlayOff;
    // Vibration (0.3.2): the controller rumbles with the game's camera shakes
    // (crashes, takedowns, nitro...). Changes live.
    int vibration = kVibrationNormal;
    // FMOD master volume in percent (50-100). At 100 the game's mix goes past
    // the 16-bit limit at loud moments (up to 1.8x full scale in a race) and is
    // cut off; at 70 it hardly ever does.
    int sound_volume = 70;
    bool gl_thread = true;   // run GL calls on their own thread (takes effect at start-up)
    bool jit_cache = true;   // remember translated code and translate it ahead (core/jit_cache.h; start-up)
    bool direct_memory = true;  // Switch JIT: direct guest memory access (start-up)
};

// The settings in use (loaded from the file at start-up).
Values& get();
// The values the game started with (quality only takes effect at start-up).
const Values& at_startup();
// The recommended defaults ("Restore recommended settings").
Values defaults();

void load();  // reads settings.ini; writes it with defaults if it is missing
void save();

std::string path();

}  // namespace game::prefs
