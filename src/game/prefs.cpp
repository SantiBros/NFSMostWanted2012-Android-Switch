#include "game/prefs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "core/common.h"
#include "game/paths.h"
#include "platform/platform.h"

namespace game::prefs {

namespace {
Values g_values;
Values g_startup;

bool parse_bool(const char* v) {
    return !std::strcmp(v, "on") || !std::strcmp(v, "1") || !std::strcmp(v, "true") || !std::strcmp(v, "yes");
}

// "1785" / "460.8" (MHz) -> kHz; "stock" -> kClockStock; "auto" -> kClockAuto.
int parse_clock(const char* v, bool allow_auto) {
    if (!std::strcmp(v, "stock") || !std::strcmp(v, "0")) return kClockStock;
    if (!std::strcmp(v, "auto")) return allow_auto ? kClockAuto : kClockStock;
    double mhz = std::atof(v);
    if (mhz < 50 || mhz > 3000) return allow_auto ? kClockAuto : kClockStock;
    return (int)std::lround(mhz * 1000);
}

std::string clock_text(int khz) {
    if (khz == kClockStock) return "stock";
    if (khz == kClockAuto) return "auto";
    char b[32];
    if (khz % 1000) std::snprintf(b, sizeof b, "%.1f", khz / 1000.0);
    else std::snprintf(b, sizeof b, "%d", khz / 1000);
    return b;
}

const char* quality_text(int q) {
    switch (q) {
    case kQualityHighest: return "highest";
    case kQualityHigh: return "high";
    case kQualityLow: return "low";
    default: return "medium";
    }
}

int parse_quality(const char* v) {
    if (!std::strcmp(v, "highest")) return kQualityHighest;
    if (!std::strcmp(v, "high")) return kQualityHigh;
    if (!std::strcmp(v, "medium")) return kQualityMedium;
    if (!std::strcmp(v, "low")) return kQualityLow;
    return kQualityMedium;
}

// Hand-edited clock values are snapped to the rates the console uses.
int snap(int khz, std::initializer_list<int> allowed) {
    if (khz <= 0) return khz;  // stock / auto
    int best = *allowed.begin();
    for (int a : allowed)
        if (std::abs(a - khz) < std::abs(best - khz)) best = a;
    return best;
}

const char* overlay_text(int o) { return o == kOverlayFps ? "fps" : o == kOverlayFull ? "full" : "off"; }
int parse_overlay(const char* v) {
    if (!std::strcmp(v, "fps")) return kOverlayFps;
    if (!std::strcmp(v, "full")) return kOverlayFull;
    return kOverlayOff;
}
}  // namespace

Values& get() { return g_values; }
const Values& at_startup() { return g_startup; }
Values defaults() { return Values{}; }
std::string path() { return paths::settings_file(); }

void save() {
    const Values& v = g_values;
    FILE* f = std::fopen(path().c_str(), "wb");
    if (!f) {
        LOGW("settings: cannot write %s", path().c_str());
        return;
    }
    std::fprintf(f,
                 "# NFS Most Wanted 2012 loader settings\n"
                 "\n"
                 "# Gyro steering: tilt the console (or controller) like a steering wheel.\n"
                 "# Also switched on/off in the game: Options > Controls > TILT TO STEER.\n"
                 "gyro_steering=%s\n"
                 "# Tilt in degrees for full steering lock (smaller = more sensitive), 10-60.\n"
                 "gyro_max_angle=%.0f\n"
                 "# Set to on if the car steers the wrong way.\n"
                 "gyro_invert=%s\n"
                 "\n"
                 "# Performance. These are also in the debug menu: hold ZL + ZR and press Minus.\n"
                 "# Frame rate limit: 30, 60 or off. The game is made for 30.\n"
                 "fps_limit=%s\n"
                 "# Wait for the screen refresh before showing a frame (no tearing).\n"
                 "vsync=%s\n"
                 "# Resolution of the 3D scene in races, in percent of the screen (50-100).\n"
                 "# Menus and the race HUD always use the full resolution.\n"
                 "scene_resolution=%d\n"
                 "# The game's detail level: highest, high, medium (recommended) or low.\n"
                 "# Takes effect the next time the game starts.\n"
                 "quality=%s\n"
                 "# Switch clocks in MHz, or stock (the console's normal clocks; recommended).\n"
                 "# cpu_clock: 1020, 1224, 1428, 1581 or 1785. Higher = faster game, more heat.\n"
                 "# gpu_clock: auto (460.8 handheld, stock docked), 307.2, 384, 460.8, 614.4 or 768.\n"
                 "# mem_clock: 1331.2 or 1600.\n"
                 "cpu_clock=%s\n"
                 "gpu_clock=%s\n"
                 "mem_clock=%s\n"
                 "# Frame rate counter: off, fps or full.\n"
                 "overlay=%s\n"
                 "# Run the game's graphics calls on their own CPU core (next start).\n"
                 "gl_thread=%s\n"
                 "# Switch: translated game code reads and writes its memory directly instead of\n"
                 "# through a lookup table (faster). Turn off if the game crashes (next start).\n"
                 "direct_memory=%s\n",
                 v.gyro_steering ? "on" : "off", v.gyro_max_angle, v.gyro_invert ? "on" : "off",
                 v.fps_limit ? std::to_string(v.fps_limit).c_str() : "off", v.vsync ? "on" : "off", v.scene_scale,
                 quality_text(v.quality), clock_text(v.cpu_khz).c_str(), clock_text(v.gpu_khz).c_str(),
                 clock_text(v.mem_khz).c_str(), overlay_text(v.overlay), v.gl_thread ? "on" : "off",
                 v.direct_memory ? "on" : "off");
    std::fclose(f);
}

void load() {
    Values& v = g_values;
    v = Values{};
    FILE* f = std::fopen(path().c_str(), "rb");
    if (!f) {
        save();  // write the defaults so the file can be edited
        g_startup = v;
        return;
    }
    bool has_perf = false;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        char key[64] = {}, val[64] = {};
        if (line[0] == '#' || std::sscanf(line, " %63[^= ] = %63s", key, val) != 2) continue;
        if (!std::strcmp(key, "gyro_steering")) v.gyro_steering = parse_bool(val);
        else if (!std::strcmp(key, "gyro_max_angle")) v.gyro_max_angle = std::clamp((float)std::atof(val), 10.f, 60.f);
        else if (!std::strcmp(key, "gyro_invert")) v.gyro_invert = parse_bool(val);
        else if (!std::strcmp(key, "fps_limit")) {
            int n = std::atoi(val);
            v.fps_limit = n >= 45 ? 60 : n > 0 ? 30 : 0;
            has_perf = true;
        } else if (!std::strcmp(key, "vsync")) v.vsync = parse_bool(val);
        else if (!std::strcmp(key, "scene_resolution")) v.scene_scale = std::clamp(std::atoi(val), 50, 100);
        else if (!std::strcmp(key, "quality")) v.quality = parse_quality(val);
        else if (!std::strcmp(key, "cpu_clock")) v.cpu_khz = parse_clock(val, false);
        else if (!std::strcmp(key, "gpu_clock")) v.gpu_khz = parse_clock(val, true);
        else if (!std::strcmp(key, "mem_clock")) v.mem_khz = parse_clock(val, false);
        else if (!std::strcmp(key, "overlay")) v.overlay = parse_overlay(val);
        else if (!std::strcmp(key, "gl_thread")) v.gl_thread = parse_bool(val);
        else if (!std::strcmp(key, "direct_memory")) v.direct_memory = parse_bool(val);
    }
    std::fclose(f);
    v.cpu_khz = snap(v.cpu_khz, {1020000, 1224000, 1428000, 1581000, 1785000});
    v.gpu_khz = snap(v.gpu_khz, {307200, 384000, 460800, 614400, 768000});
    v.mem_khz = snap(v.mem_khz, {1331200, 1600000});
    if (!has_perf) save();  // older file (gyro only): add the performance section
    g_startup = v;
    LOGI("settings: fps limit %s, vsync %s, 3D resolution %d%%, quality %s, clocks cpu %s gpu %s mem %s",
         v.fps_limit ? std::to_string(v.fps_limit).c_str() : "off", v.vsync ? "on" : "off", v.scene_scale,
         quality_text(v.quality), clock_text(v.cpu_khz).c_str(), clock_text(v.gpu_khz).c_str(),
         clock_text(v.mem_khz).c_str());
}

}  // namespace game::prefs
