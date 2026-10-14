// Sound: FMOD Ex mixes in the game's code; on Android its Java helper
// (org.fmod.FMODAudioDevice) pulls PCM through fmodProcess() and feeds an
// AudioTrack. This is the same loop, feeding the platform's audio output.
//
// On the Switch the loop runs on core 1 above the game's worker threads: the
// mixing is real-time work, and on core 2 (0.2.5) it had to wait whenever the
// GL thread, which has that core first, was busy, so the sound broke up.
#include "game/audio.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "core/cpu.h"
#include "core/elf.h"
#include "core/hook.h"
#include "core/memory.h"
#include "core/thunks.h"
#include "game/prefs.h"
#include "hle/jni/jni.h"
#include "platform/platform.h"
#include "platform/thread.h"

namespace game::audio {

namespace {
using V = jni::Value;
std::atomic<bool> g_running{false};
using Clock = std::chrono::steady_clock;
std::atomic<uint64_t> g_mix_ns{0}, g_mix_calls{0};
std::atomic<int> g_block_frames{0}, g_rate{0};

// Sound volume: the volume of FMOD's master channel group, set before FMOD
// turns its floating-point mix into 16-bit samples. The game's own mix is so
// loud that it often goes past the 16-bit limit and is cut off there
// (clipping: a harsh crackle on loud engines, crashes and music); a lower
// master volume leaves room for it.
std::atomic<u32> g_fmod_system{0};  // FMOD::System* (guest), once FMOD_System_Create returned it
u32 g_fn_master_group = 0, g_fn_set_volume = 0;
u32 g_out_word = 0;  // guest memory for the channel group pointer
int g_applied_volume = -1;

int info(jni::jref dev, int what) {
    return jni::call_native("org/fmod/FMODAudioDevice", "fmodGetInfo", "(I)I", dev, {V::I(what)}).i();
}

void run() {
    platform::set_thread_name("sound");
    platform::boost_current_thread();
    cpu::attach_current_thread(512 * 1024);
    jni::jref dev = jni::new_object("org/fmod/FMODAudioDevice");
    jni::jref buf = 0;
    u32 bytes = 0;
    int rate = 0;
    bool open = false;
    while (g_running) {
        if (!open) {
            rate = info(dev, 0);  // FMOD_INFO_SAMPLERATE
            if (rate <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            int len = info(dev, 1);  // DSP buffer length (frames)
            int num = info(dev, 2);  // number of DSP buffers
            bytes = (u32)len * 2 * 2;
            buf = jni::new_object("java/nio/ByteBuffer");
            jni::get(buf)->data = mem::heap_calloc(bytes, 1);
            jni::get(buf)->length = bytes;
            open = platform::audio_open(rate, 2, len * num);
            g_block_frames = len;
            g_rate = rate;
            LOGI("FMOD audio: %d Hz, %d frames x %d buffers -> %s", rate, len, num, open ? "playing" : "no output");
            if (!open) break;
        }
        if (info(dev, 3) != 1) {  // FMOD_INFO_MIXERRUNNING
            platform::audio_pause();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const auto t0 = Clock::now();
        jni::call_native("org/fmod/FMODAudioDevice", "fmodProcess", "(Ljava/nio/ByteBuffer;)I", dev, {V::L(buf)});
        g_mix_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
        g_mix_calls++;
        platform::audio_write(mem::ptr<const s16>(jni::get(buf)->data), (int)(bytes / 4));
    }
    cpu::detach_current_thread();
}
}  // namespace

void start() {
    if (g_running.exchange(true)) return;
    if (!jni::has_native("org/fmod/FMODAudioDevice", "fmodProcess")) {
        LOGW("FMOD Java audio bridge not found; sound disabled");
        g_running = false;
        return;
    }
    platform::detach_thread(platform::spawn_thread(run, 1 << 20, 1));
}

void stop() { g_running = false; }

void install() {
    elf::Module* m = elf::find("libfmodex.so");
    if (!m) return;
    const u32 create = m->find_export("FMOD_System_Create");
    g_fn_master_group = m->find_export("FMOD_System_GetMasterChannelGroup");
    g_fn_set_volume = m->find_export("FMOD_ChannelGroup_SetVolume");
    if (!create || !g_fn_master_group || !g_fn_set_volume) {
        LOGW("sound volume: FMOD functions not found; volume setting off");
        return;
    }
    // FMOD_RESULT FMOD_System_Create(FMOD_SYSTEM** system): after it returns,
    // r0 = result, r1 = the pointer it wrote the new system to.
    const u32 stub = thunks::add("fmod_system_created", [](cpu::GuestThread& t) {
        const auto& R = t.regs();
        if (R[0] == 0 && R[1]) g_fmod_system = mem::r32(R[1]);
    });
    if (!hook::inline_post(create, stub)) {
        LOGW("sound volume: could not hook FMOD_System_Create; volume setting off");
        return;
    }
    g_out_word = mem::static_alloc(4, 4);
}

void frame() {
    const u32 sys = g_fmod_system.load();
    if (!sys || !g_out_word) return;
    static const char* env = std::getenv("NFSMW_SOUND_VOLUME");  // tests: overrides the setting
    const int want = env ? std::atoi(env) : prefs::get().sound_volume;
    if (want == g_applied_volume) return;
    auto& t = cpu::current();
    mem::w32(g_out_word, 0);
    // Fails (and is tried again next frame) until the game has initialised FMOD.
    if ((u32)t.call(g_fn_master_group, {sys, g_out_word}) != 0 || !mem::r32(g_out_word)) return;
    const float v = (float)want / 100.f;
    const u32 r = (u32)t.call(g_fn_set_volume, {mem::r32(g_out_word), bit_cast_<u32>(v)});  // softfp: float in r1
    g_applied_volume = want;
    LOGI("sound volume: %d%%%s", want, r ? " (FMOD refused it)" : "");
}

std::string summary() {
    static uint64_t last_ns = 0, last_calls = 0;
    const uint64_t ns = g_mix_ns.load(), calls = g_mix_calls.load();
    const int frames = g_block_frames.load(), rate = g_rate.load();
    if (calls == last_calls || rate <= 0) return "";
    const double mix_ms = (double)(ns - last_ns) / 1e6 / (double)(calls - last_calls);
    last_ns = ns;
    last_calls = calls;
    const platform::AudioStats st = platform::audio_stats();
    char out[160];
    std::snprintf(out, sizeof out, "sound: mixing %.1f ms per %.1f ms of sound, %u dropouts, %d ms queued", mix_ms,
                  1000.0 * frames / rate, st.dropouts, st.queue_ms);
    return out;
}

}  // namespace game::audio
