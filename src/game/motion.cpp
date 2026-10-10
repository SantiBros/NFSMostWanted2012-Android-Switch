#include "game/motion.h"

#include "game/prefs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace game::motion {

namespace {
bool g_seen = false;
bool g_have_filter = false;
float g_filtered = 0;  // filtered tilt angle, degrees
std::chrono::steady_clock::time_point g_last;
}  // namespace

void load_settings() {
    // settings.ini is read once by game::prefs::load() (main.cpp).
    const auto& p = prefs::get();
    LOGI("gyro steering: %s (full lock at %.0f degrees%s)", p.gyro_steering ? "on" : "off", p.gyro_max_angle,
         p.gyro_invert ? ", inverted" : "");
}

bool enabled() { return prefs::get().gyro_steering; }

void set_enabled(bool on) {
    prefs::get().gyro_steering = on;
    reset();
    prefs::save();
    LOGI("gyro steering turned %s", on ? "on" : "off");
}

bool sensor_seen() { return g_seen; }

void reset() { g_have_filter = false; }

bool steering(const platform::InputState& s, float& out) {
    if (!s.motion) return false;
    float mag = std::sqrt(s.gx * s.gx + s.gy * s.gy + s.gz * s.gz);
    if (mag < 0.3f) return false;  // no usable gravity reading (free fall, sensor glitch)
    if (!g_seen) {
        g_seen = true;
        LOGI("motion sensor: reporting (gravity %.2f %.2f %.2f)", s.gx, s.gy, s.gz);
    }
    const auto& p = prefs::get();
    if (!p.gyro_steering) return false;

    // How far the right side is tilted down, whatever the pitch: the angle
    // between the device's x axis and the horizontal. Works the same for a
    // console held upright like a steering wheel and for a controller held
    // flat and rolled.
    float angle = std::asin(std::clamp(s.gx / mag, -1.f, 1.f)) * 57.29578f;
    if (p.gyro_invert) angle = -angle;

    // Low-pass filter (time constant ~60 ms) against hand shake.
    auto now = std::chrono::steady_clock::now();
    float dt = g_have_filter ? std::chrono::duration<float>(now - g_last).count() : 1.f;
    g_last = now;
    float k = std::clamp(dt / (0.06f + dt), 0.f, 1.f);
    g_filtered = g_have_filter ? g_filtered + (angle - g_filtered) * k : angle;
    g_have_filter = true;

    // Small dead zone around level, then linear up to full lock.
    constexpr float kDead = 2.f;
    float a = std::fabs(g_filtered) <= kDead ? 0.f : g_filtered - std::copysign(kDead, g_filtered);
    out = std::clamp(a / (p.gyro_max_angle - kDead), -1.f, 1.f);
    return true;
}

}  // namespace game::motion
