#include "game/rumble.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "core/cpu.h"
#include "core/elf.h"
#include "core/memory.h"
#include "core/thunks.h"
#include "game/prefs.h"
#include "platform/platform.h"

// The game's camera shakes (libapp.so 1.3.128):
//   +0x91DF0  CameraShakes::Start(this, int type, float seconds)  (seconds 0: until Stop)
//   +0x91EB4  CameraShakes::Stop(this, int type)
// `this` holds a vector of shared_ptr<CameraShake> at +4/+8; each CameraShake
// has its type at +4 and, set just before Start by the same message (id 0x3F6,
// handler +0x98938), two shake speeds at +0x14/+0x18 and two shake lengths
// (how far the camera moves) at +0x1C/+0x20.
//   type 0, crashes (+0xC014C): the impact against three thresholds picks the
//     Hard, Medium or Light collision shake from the game's tuning (below);
//   type 1, nitro (+0xEA038): until the nitro ends (+0xE9C0C stops it).
// Both functions get a small entry hook that tells this module and then runs
// the game's code unchanged.
//
// The tuning object (+0x341FB8 returns it; the pointer is kept 0xC bytes into
// a static block): per collision shake {active time, speed max, speed min,
// length max, length min} at +0x190 (Hard), +0x1A4 (Medium), +0x1B8 (Light).
// A crash's shake is matched against those three to tell how hard it was.
//
// Nitro: the camera shake for it has not been seen on Android, so the port also
// watches the player's nitro gauge. The HUD's nitro bar (Update at +0x1E11F0)
// keeps the player's nitro component at +0xF4 (its shared_ptr count block at
// +0xFC), whose nitro level is the float at +0xBC; while that level goes down,
// the player is using nitro.

namespace game::rumble {

namespace {

constexpr u32 kStart = 0x91DF0, kStop = 0x91EB4;
constexpr u32 kTuningGet = 0x341FB8;  // returns the tuning object
constexpr u32 kNitroBarUpdate = 0x1E11F0;
constexpr u32 kCrash = 0, kNitro = 1;  // shake types

struct Shake {
    u32 type = 0;
    float strength = 0;   // 0..1
    float total = 0;      // seconds, 0 = until stopped
    float left = 0;       // seconds left (timed shakes)
    float age = 0;        // seconds since it started
};

std::mutex g_mutex;
std::map<u32, Shake> g_shakes;  // by shake type
bool g_hold = false;
float g_pulse = 0;              // test pulse, seconds left
bool g_log = false;
u32 g_tuning_slot = 0;          // guest address of the tuning object pointer
// The player's nitro gauge, as the HUD last saw it.
u32 g_nitro_comp = 0;
float g_nitro_level = 0;
bool g_nitro_on = false;
std::chrono::steady_clock::time_point g_nitro_used;  // last time the level went down

bool matches(u32 at, std::initializer_list<u32> words) {
    u32 a = at;
    for (u32 w : words) {
        if (mem::r32(a) != w) return false;
        a += 4;
    }
    return true;
}

u32 emit(const std::vector<u32>& c) {
    const u32 at = mem::static_alloc((u32)c.size() * 4, 8);
    for (size_t i = 0; i < c.size(); i++) mem::w32(at + (u32)i * 4, c[i]);
    return at;
}

// At `f`: save r0-r3, call `thunk`, restore, run the two instructions the jump
// replaced and carry on at f + 8.
void entry_hook(u32 f, u32 thunk) {
    const u32 tramp = emit({
        0xE92D500F,        // push {r0-r3, ip, lr}
        0xE59FC014,        // ldr ip, [pc, #20]   (thunk)
        0xE12FFF3C,        // blx ip
        0xE8BD500F,        // pop {r0-r3, ip, lr}
        mem::r32(f),       // the game's first two instructions
        mem::r32(f + 4),
        0xE51FF004,        // ldr pc, [pc, #-4]
        f + 8,
        thunk,
    });
    mem::w32(f, 0xE51FF004);  // ldr pc, [pc, #-4]
    mem::w32(f + 4, tramp);
}

bool in_guest(u32 a, u32 len) { return a >= mem::NULL_GUARD_END && a <= mem::g_size && len <= mem::g_size - a; }

float as_float(u32 bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

float guest_float(u32 a) { return as_float(mem::r32(a)); }

// How hard a crash was, 0..1, from its camera shake: the closest of the game's
// Hard, Medium and Light collision shakes (by active time and shake length).
float crash_strength(float seconds, float length) {
    static const struct { u32 off; float strength; const char* name; } kLevels[] = {
        {0x190, 1.0f, "hard"}, {0x1A4, 0.65f, "medium"}, {0x1B8, 0.35f, "light"}};
    const u32 tuning = g_tuning_slot && in_guest(g_tuning_slot, 4) ? mem::r32(g_tuning_slot) : 0;
    if (tuning && in_guest(tuning + 0x190, 0x3C)) {
        float best = 1e9f, strength = 0.35f;
        const char* name = "light";
        for (const auto& l : kLevels) {
            const float t = guest_float(tuning + l.off);
            const float len = std::max(guest_float(tuning + l.off + 0xC), guest_float(tuning + l.off + 0x10));
            if (!std::isfinite(t) || !std::isfinite(len)) continue;
            const float d = std::fabs(t - seconds) / std::max(t, 0.05f) + std::fabs(len - length) / std::max(len, 0.01f);
            if (d < best) best = d, strength = l.strength, name = l.name;
        }
        if (g_log) LOGI("rumble: crash shake %.2f s, length %.3f: %s", seconds, length, name);
        return strength;
    }
    // No tuning to compare with: by how long the camera shakes.
    return seconds >= 0.4f ? 1.0f : seconds >= 0.2f ? 0.65f : 0.35f;
}

void on_start(u32 self, u32 type, float seconds) {
    // The shake's speeds and lengths, set by the same message just before.
    float v1 = 0, v2 = 0, l1 = 0, l2 = 0;
    if (in_guest(self + 4, 8)) {
        const u32 begin = mem::r32(self + 4), end = mem::r32(self + 8);
        for (u32 e = begin; e < end && end - begin < 4096 && in_guest(e, 8); e += 8) {
            const u32 p = mem::r32(e);
            if (!in_guest(p, 0x24) || mem::r32(p + 4) != type) continue;
            v1 = guest_float(p + 0x14), v2 = guest_float(p + 0x18);
            l1 = guest_float(p + 0x1C), l2 = guest_float(p + 0x20);
            break;
        }
    }
    if (!(seconds > 0)) seconds = 0;  // negative (or NaN): until stopped
    const float length = std::max(std::isfinite(l1) ? l1 : 0.f, std::isfinite(l2) ? l2 : 0.f);
    if (g_log)
        LOGI("rumble: camera shake %u start, %.2f s, speeds %.3f %.3f, lengths %.3f %.3f", type, seconds, v1, v2, l1, l2);
    Shake s;
    s.type = type;
    if (type == kCrash && seconds > 0) {
        // A crash: a thump, longer and stronger the harder it was.
        s.strength = crash_strength(seconds, length);
        s.total = std::clamp(seconds, 0.12f + 0.3f * s.strength, 0.6f);
    } else if (type == kNitro || seconds <= 0) {
        // Nitro (and anything else that lasts until stopped): a steady buzz.
        s.strength = 0.5f;
        s.total = 0;
    } else {
        s.strength = 0.3f;  // anything else: a light bump
        s.total = std::clamp(seconds, 0.1f, 0.4f);
    }
    s.left = s.total;
    std::lock_guard lk(g_mutex);
    Shake& cur = g_shakes[type];
    // A lighter crash does not cut a stronger one short.
    if (s.total > 0 && cur.total > 0 && cur.left > 0 && cur.strength * cur.left / cur.total > s.strength) return;
    cur = s;
}

// The HUD's nitro bar, once per frame during a race (game thread).
void on_nitro_bar(u32 bar) {
    if (!in_guest(bar + 0xF4, 0xC)) return;
    const u32 comp = mem::r32(bar + 0xF4), ctrl = mem::r32(bar + 0xFC);
    // Only while the bar's shared_ptr still keeps the component alive.
    if (!in_guest(comp, 0xC0) || !in_guest(ctrl, 8) || mem::r32(ctrl + 4) == 0) return;
    const float level = guest_float(comp + 0xBC);
    if (!std::isfinite(level)) return;
    std::lock_guard lk(g_mutex);
    if (comp != g_nitro_comp) {
        g_nitro_comp = comp;
        g_nitro_level = level;
        if (g_log) LOGI("rumble: player's nitro gauge at %08X, level %.3f", comp, level);
        return;
    }
    if (level < g_nitro_level - 1e-5f) {
        if (!g_nitro_on && g_log) LOGI("rumble: nitro on (level %.3f)", level);
        g_nitro_on = true;
        g_nitro_used = std::chrono::steady_clock::now();
    }
    g_nitro_level = level;
}

void on_stop(u32 type) {
    if (g_log) LOGI("rumble: camera shake %u stop", type);
    std::lock_guard lk(g_mutex);
    g_shakes.erase(type);
}

float multiplier() {
    switch (prefs::get().vibration) {
    case prefs::kVibrationOff: return 0;
    case prefs::kVibrationLow: return 0.5f;
    case prefs::kVibrationStrong: return 1.0f;
    default: return 0.75f;
    }
}

}  // namespace

void install() {
    g_log = std::getenv("NFSMW_RUMBLE_LOG") != nullptr;
    elf::Module* m = elf::find("libapp.so");
    if (!m) return;
    const u32 start = m->base + kStart, stop = m->base + kStop;
    // push {r4, r10, fp, lr} ; add fp, sp, #8 ; vpush {d8} / sub sp, sp, #8
    if (!matches(start, {0xE92D4C10, 0xE28DB008, 0xED2D8B02, 0xE24DD008}) ||
        !matches(stop, {0xE92D4C10, 0xE28DB008, 0xE24DD008, 0xE1A02001})) {
        LOGW("vibration: the game's camera shake code is not where expected; no vibration");
        return;
    }
    const u32 t_start = thunks::add("rumble_shake_start", [](cpu::GuestThread& t) {
        on_start(t.regs()[0], t.regs()[1], as_float(t.regs()[2]));
    });
    const u32 t_stop = thunks::add("rumble_shake_stop", [](cpu::GuestThread& t) { on_stop(t.regs()[1]); });
    entry_hook(start, t_start);
    entry_hook(stop, t_stop);
    // push {r4, r5, r6, r7, fp, lr} ; add fp, sp, #0x10 ; vpush {d8} ; sub sp, sp, #8
    const u32 bar = m->base + kNitroBarUpdate;
    if (matches(bar, {0xE92D48F0, 0xE28DB010, 0xED2D8B02, 0xE24DD008}))
        entry_hook(bar, thunks::add("rumble_nitro_bar", [](cpu::GuestThread& t) { on_nitro_bar(t.regs()[0]); }));
    else
        LOGW("vibration: the game's nitro bar is not where expected; nitro only from camera shakes");
    // The tuning object's pointer, from the getter's code:
    //   +0x20 ldr r0, [pc, #0x9C] ; +0x24 add r0, pc, r0 ; +0x28 ldr r0, [r0, #0xC]
    const u32 get = m->base + kTuningGet;
    if (matches(get + 0x20, {0xE59F009C, 0xE08F0000, 0xE590000C}))
        g_tuning_slot = get + 0x24 + 8 + mem::r32(get + 0x20 + 8 + 0x9C) + 0xC;
    else
        LOGW("vibration: the game's tuning is not where expected; crashes are graded by their length");
    // NFSMW_PROBE=<off>,<off>...: log every call of those libapp.so functions
    // (debugging; each must start with push {...} ; add fp, sp, #n).
    if (const char* probe = std::getenv("NFSMW_PROBE")) {
        static std::vector<u32> offs;
        for (const char* p = probe; *p;) {
            char* end = nullptr;
            const u32 off = (u32)std::strtoul(p, &end, 16);
            if (end == p) break;
            offs.push_back(off);
            p = *end ? end + 1 : end;
        }
        for (size_t i = 0; i < offs.size(); i++) {
            const u32 f = m->base + offs[i];
            if ((mem::r32(f) & 0xFFFF0000) != 0xE92D0000 || (mem::r32(f + 4) & 0xFFFFF000) != 0xE28DB000) {
                LOGW("probe: +0x%X does not start with push / add fp", offs[i]);
                continue;
            }
            const u32 off = offs[i];
            entry_hook(f, thunks::add("probe_" + std::to_string(off), [off](cpu::GuestThread& t) {
                // NFSMW_PROBE_ONCE=1: each (function, r0) once, with r0's first
                // NFSMW_PROBE_DUMP words.
                static const bool once = std::getenv("NFSMW_PROBE_ONCE") != nullptr;
                static const int dump = std::getenv("NFSMW_PROBE_DUMP") ? std::atoi(std::getenv("NFSMW_PROBE_DUMP")) : 0;
                static std::mutex mx;
                static std::map<u64, int> seen;
                {
                    std::lock_guard lk(mx);
                    if (once && seen[((u64)off << 32) | t.regs()[0]]++) return;
                }
                LOGI("probe: +0x%X r0=%08X r1=%08X r2=%08X r3=%08X", off, t.regs()[0], t.regs()[1], t.regs()[2],
                     t.regs()[3]);
                const u32 r0 = t.regs()[0];
                for (int w = 0; w < dump && in_guest(r0, (u32)dump * 4); w += 8) {
                    char line[160];
                    int n = std::snprintf(line, sizeof line, "probe:   +%03X", w * 4);
                    for (int k = 0; k < 8 && w + k < dump; k++)
                        n += std::snprintf(line + n, sizeof line - n, " %08X", mem::r32(r0 + (w + k) * 4));
                    LOGI("%s", line);
                }
            }));
        }
    }
    LOGI("vibration: %s (crashes and nitro)", prefs::get().vibration == prefs::kVibrationOff ? "off" : "on");
}

void frame(float dt) {
    dt = std::clamp(dt, 0.f, 0.1f);
    float low = 0, high = 0;
    {
        std::lock_guard lk(g_mutex);
        for (auto it = g_shakes.begin(); it != g_shakes.end();) {
            Shake& s = it->second;
            s.age += dt;
            if (s.total > 0) {
                // A crash: a thump that fades out quickly, mostly low rumble.
                const float env = std::pow(std::max(s.left, 0.f) / s.total, 1.5f);
                low = std::max(low, s.strength * env);
                high = std::max(high, 0.45f * s.strength * env);
                s.left -= dt;
                if (s.left <= 0) {
                    it = g_shakes.erase(it);
                    continue;
                }
            } else {
                // Nitro: a steady buzz, mostly high rumble, faded out if
                // nothing stops it within a few seconds (a pause, say).
                const float env = s.age < 8 ? 1.f : std::max(0.f, 1.f - (s.age - 8));
                high = std::max(high, 0.7f * s.strength * env);
                low = std::max(low, 0.25f * s.strength * env);
                if (env <= 0) {
                    it = g_shakes.erase(it);
                    continue;
                }
            }
            ++it;
        }
        if (g_nitro_on) {
            // Still going down within the last moment: nitro is on.
            if (std::chrono::steady_clock::now() - g_nitro_used < std::chrono::milliseconds(250)) {
                high = std::max(high, 0.35f);
                low = std::max(low, 0.125f);
            } else {
                g_nitro_on = false;
                if (g_log) LOGI("rumble: nitro off (level %.3f)", g_nitro_level);
            }
        }
        if (g_pulse > 0) {
            low = std::max(low, 0.8f), high = std::max(high, 0.4f);
            g_pulse -= dt;
        }
        if (g_hold && g_pulse <= 0) low = high = 0;
    }
    const float k = multiplier();
    platform::set_rumble(low * k, high * k);
}

void hold(bool on) {
    std::lock_guard lk(g_mutex);
    g_hold = on;
    if (on) {
        g_shakes.clear();
        g_nitro_on = false;
    }
}

void test_pulse() {
    std::lock_guard lk(g_mutex);
    g_pulse = 0.25f;
}

}  // namespace game::rumble
