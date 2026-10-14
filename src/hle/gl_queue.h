// Threaded GL: the game's GL calls are recorded on its own thread and run on a
// separate GL thread that owns the context.
//
// The game does all of its rendering on its main thread, between its own
// game code. Without this, the time the GL driver spends on each call (state
// validation, command building, the wait for the screen refresh in the buffer
// swap) adds to every frame. With it, the main thread only writes the call
// and its arguments into a queue and carries on; the GL thread, on another CPU
// core, runs the calls in order. Data passed by pointer (uniform values,
// buffer and texture data) is copied into the queue, because the game may
// reuse that memory as soon as the call returns. Calls that return something
// (glGet*, glGen*, glGetError...) and very large uploads wait for the GL
// thread instead. In a race the game makes only fire-and-forget calls, so the
// main thread never waits except for the previous frame's buffer swap (at most
// one frame is queued ahead).
#pragma once

#include <cstddef>
#include <cstring>
#include <functional>
#include <new>
#include <tuple>
#include <utility>

#include "core/common.h"

namespace hle::glq {

// True once start() moved the context to the GL thread.
bool active();
// Start the GL thread and hand it the current context. Call on the thread
// that created the context. Returns false if threading could not be started
// (calls then keep running on the calling thread).
bool start(int core = -1);

// A recorded command: runs on the GL thread with a pointer to the bytes that
// were stored with it.
using CmdFn = void (*)(const void* data);
// Reserve `len` bytes for a command's data in the queue; fill them, then
// commit(). Only one command may be open at a time (single producer).
void* begin(CmdFn fn, size_t len);
void commit();

// Uploads larger than this run with sync() (no copy) instead of being queued.
constexpr size_t kMaxPayload = 1u << 20;

// Run f on the GL thread after everything queued so far, and wait for it.
void sync(const std::function<void()>& f);
// Wait until everything queued so far has run.
void finish();
// Queue the buffer swap. Waits until the previous frame's swap has finished,
// so at most one frame is in flight.
void swap(void (*do_swap)());
// Work for the calling (game) thread while swap() would otherwise wait: called
// repeatedly until the wait is over or it returns false.
void set_idle_work(bool (*step)());

// Statistics: share of time the GL thread was busy (0..1, not counting buffer
// swaps, which mostly wait for the display) since the last call, and the time
// the main thread waited for the GL thread (ms) since the last call, and the
// time the GL thread spent in buffer swaps (ms; waiting for the GPU to finish
// and for the display) since the last call.
void stats(float& busy, float& waited_ms, float* swap_ms = nullptr);

// ---- helpers ------------------------------------------------------------------
template <class... A>
struct Packed {
    void (*fn)(A...);
    std::tuple<A...> args;
};
template <class... A>
void run_packed(const void* data) {
    const auto* p = static_cast<const Packed<A...>*>(data);
    std::apply(p->fn, p->args);
}

// Call fn(args...) on the GL thread (queued) or directly when not active.
template <class... A, class... B>
inline void call(void (*fn)(A...), B... b) {
    if (!active()) {
        fn(static_cast<A>(b)...);
        return;
    }
    void* mem = begin(&run_packed<A...>, sizeof(Packed<A...>));
    new (mem) Packed<A...>{fn, std::tuple<A...>(static_cast<A>(b)...)};
    commit();
}

// Call fn(args..., data) where data is `len` bytes that must be copied now.
// Args holds the scalar arguments; Fn receives (const Args&, const void* data).
template <class Args>
struct WithData {
    void (*fn)(const Args&, const void*);
    Args args;
    size_t len;
};
template <class Args>
void run_with_data(const void* data) {
    const auto* w = static_cast<const WithData<Args>*>(data);
    const void* payload = w->len ? reinterpret_cast<const u8*>(w) + ((sizeof(WithData<Args>) + 15) & ~size_t(15)) : nullptr;
    w->fn(w->args, payload);
}
template <class Args>
inline void call_data(void (*fn)(const Args&, const void*), const Args& args, const void* data, size_t len) {
    if (!data) len = 0;
    if (!active()) {
        fn(args, data);
        return;
    }
    if (len > kMaxPayload) {  // large: run it now on the GL thread, straight from guest memory
        sync([&] { fn(args, data); });
        return;
    }
    const size_t head = (sizeof(WithData<Args>) + 15) & ~size_t(15);
    u8* mem = static_cast<u8*>(begin(&run_with_data<Args>, head + len));
    new (mem) WithData<Args>{fn, args, len};
    if (len) std::memcpy(mem + head, data, len);
    commit();
}

// Run fn(args...) on the GL thread and return its result.
template <class R, class... A, class... B>
inline R call_sync(R (*fn)(A...), B... b) {
    if (!active()) return fn(static_cast<A>(b)...);
    R r{};
    sync([&] { r = fn(static_cast<A>(b)...); });
    return r;
}
template <class... A, class... B>
inline void call_sync_void(void (*fn)(A...), B... b) {
    if (!active()) return fn(static_cast<A>(b)...);
    sync([&] { fn(static_cast<A>(b)...); });
}

}  // namespace hle::glq
