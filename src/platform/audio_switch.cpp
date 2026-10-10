// Sound output on the Switch: audout, 48 kHz stereo s16.
//
// FMOD mixes 1024 frames at 24 kHz per call (42.7 ms). Each call is converted
// to 48 kHz (platform/resampler.h) and queued as one 2048-frame buffer. The
// queue starts 4 buffers deep (171 ms: how far the sound runs behind the
// game). If the output ever runs dry (the mixer was late: a dropout), the
// queue is kept one buffer deeper from then on, up to 8 buffers (341 ms, the
// fixed queue of 0.2.5).
#include <switch.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "platform/platform.h"
#include "platform/resampler.h"

namespace platform {

namespace {
constexpr int kOutRate = 48000;
constexpr int kBufFrames = 2048;              // 42.7 ms
constexpr size_t kBufBytes = kBufFrames * 4;  // 0x2000: audout wants 0x1000-aligned buffers
constexpr int kStartBuffers = 4;
constexpr int kMaxBuffers = 8;

AudioOutBuffer g_bufs[kMaxBuffers];
std::vector<AudioOutBuffer*> g_free;  // played (or never queued): ready to fill
int g_queued = 0;                     // appended and not played yet
int g_depth = kStartBuffers;          // buffers the queue keeps
int g_in_rate = 0;
bool g_ok = false;
bool g_paused = false;                // the mixer stopped: an empty queue is not a dropout
Resampler g_resampler;
std::vector<s16> g_pending;           // converted frames not queued yet
std::atomic<u32> g_dropouts{0};
std::atomic<int> g_depth_now{kStartBuffers};

// Collect every buffer audout has finished playing (without waiting).
void take_played() {
    for (;;) {
        AudioOutBuffer* b = nullptr;
        u32 n = 0;
        if (R_FAILED(audoutGetReleasedAudioOutBuffer(&b, &n)) || n == 0 || !b) return;
        g_free.push_back(b);
        g_queued--;
    }
}

// Wait until audout has played at least one buffer.
bool wait_played() {
    AudioOutBuffer* b = nullptr;
    u32 n = 0;
    if (R_FAILED(audoutWaitPlayFinish(&b, &n, UINT64_MAX))) return false;
    if (n && b) {
        g_free.push_back(b);
        g_queued--;
    }
    take_played();
    return true;
}

void queue_buffer(const s16* frames) {
    AudioOutBuffer* b = g_free.back();
    g_free.pop_back();
    if (frames) std::memcpy(b->buffer, frames, kBufBytes);
    else std::memset(b->buffer, 0, kBufBytes);
    b->data_size = kBufBytes;
    b->data_offset = 0;
    if (R_SUCCEEDED(audoutAppendAudioOutBuffer(b))) g_queued++;
    else g_free.push_back(b);
}

int buffers_ms(int n) { return n * kBufFrames * 1000 / kOutRate; }
}  // namespace

bool audio_open(int rate, int channels, int) {
    if (channels != 2 || rate <= 0) return false;
    if (R_FAILED(audoutInitialize()) || R_FAILED(audoutStartAudioOut())) {
        LOGE("audout initialisation failed");
        return false;
    }
    for (auto& b : g_bufs) {
        void* mem = aligned_alloc(0x1000, kBufBytes);
        if (!mem) return false;
        b = AudioOutBuffer{};
        b.buffer = mem;
        b.buffer_size = kBufBytes;
        b.data_size = kBufBytes;
        g_free.push_back(&b);
    }
    g_in_rate = rate;
    g_resampler.configure(rate, kOutRate);
    while (g_queued < g_depth && !g_free.empty()) queue_buffer(nullptr);  // start with silence
    g_ok = true;
    LOGI("sound output: %d Hz to %d Hz, %d buffers of %d frames queued (%d ms)", rate, kOutRate, g_depth, kBufFrames,
         buffers_ms(g_depth));
    return true;
}

void audio_write(const s16* samples, int frames) {
    if (!g_ok) {
        // No output: still take as long as the sound would (the caller is paced by this).
        if (g_in_rate > 0) svcSleepThread((s64)frames * 1000000000ll / g_in_rate);
        return;
    }
    g_resampler.process(samples, frames, g_pending);
    const size_t per_buf = (size_t)kBufFrames * 2;
    size_t used = 0;
    while (g_pending.size() - used >= per_buf) {
        take_played();
        if (g_queued == 0 && !g_paused) {
            // The output ran dry: the mixer was late. Keep one more buffer
            // queued from now on. (The queue fills up again by itself: the
            // mixer is not held back until it is full.)
            const u32 n = ++g_dropouts;
            if (g_depth < kMaxBuffers) {
                g_depth++;
                g_depth_now = g_depth;
                LOGW("sound: dropout %u (the mixer was late); queue now %d ms", n, buffers_ms(g_depth));
            }
        }
        g_paused = false;
        while (g_queued >= g_depth || g_free.empty()) {
            if (!wait_played()) {
                LOGE("sound: audout stopped responding; sound off");
                g_ok = false;
                g_pending.clear();
                return;
            }
        }
        queue_buffer(g_pending.data() + used);
        used += per_buf;
    }
    g_pending.erase(g_pending.begin(), g_pending.begin() + (std::ptrdiff_t)used);
}

void audio_pause() { g_paused = true; }

AudioStats audio_stats() {
    AudioStats s;
    s.dropouts = g_dropouts.load();
    s.queue_ms = buffers_ms(g_depth_now.load());
    return s;
}

}  // namespace platform
