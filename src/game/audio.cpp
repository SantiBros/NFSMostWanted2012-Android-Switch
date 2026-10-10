// Sound: FMOD Ex mixes in the game's code; on Android its Java helper
// (org.fmod.FMODAudioDevice) pulls PCM through fmodProcess() and feeds an
// AudioTrack. This is the same loop, feeding the platform's audio output.
#include "game/audio.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "core/cpu.h"
#include "core/memory.h"
#include "hle/jni/jni.h"
#include "platform/platform.h"
#include "platform/thread.h"

namespace game::audio {

namespace {
using V = jni::Value;
std::atomic<bool> g_running{false};

int info(jni::jref dev, int what) {
    return jni::call_native("org/fmod/FMODAudioDevice", "fmodGetInfo", "(I)I", dev, {V::I(what)}).i();
}

void run() {
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
            LOGI("FMOD audio: %d Hz, %d frames x %d buffers -> %s", rate, len, num, open ? "playing" : "no output");
            if (!open) break;
        }
        if (info(dev, 3) != 1) {  // FMOD_INFO_MIXERRUNNING
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        jni::call_native("org/fmod/FMODAudioDevice", "fmodProcess", "(Ljava/nio/ByteBuffer;)I", dev, {V::L(buf)});
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
    platform::detach_thread(platform::spawn_thread(run, 1 << 20, 2));
}

void stop() { g_running = false; }

}  // namespace game::audio
