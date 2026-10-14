#include "hle/gl_queue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "platform/platform.h"
#include "platform/thread.h"

namespace hle::glq {

namespace {
using Clock = std::chrono::steady_clock;

// Ring buffer of commands: [Header][data, padded to 16 bytes] ...
// Positions grow forever; position % kRing is the offset in the buffer.
constexpr size_t kRing = 32u << 20;
struct alignas(16) Header {
    CmdFn fn;  // nullptr: skip to the start of the buffer
    u64 len;
};
static_assert(sizeof(Header) == 16);
constexpr size_t align16(size_t n) { return (n + 15) & ~size_t(15); }

u8* g_ring = nullptr;
std::atomic<u64> g_write{0};  // committed end (producer -> consumer)
std::atomic<u64> g_read{0};   // consumed end (consumer -> producer)
u64 g_open_pos = 0, g_open_need = 0;
bool g_active = false;

std::mutex g_mu;
std::condition_variable g_cv_data, g_cv_progress;
std::atomic<bool> g_consumer_sleeping{false};
std::atomic<bool> g_producer_waiting{false};

std::atomic<u64> g_swaps_done{0};
u64 g_swaps_queued = 0;

std::atomic<u64> g_busy_ns{0};
std::atomic<u64> g_swap_ns{0};  // part of the busy time spent in buffer swaps (waiting for the display)
u64 g_stats_swap0 = 0;
u64 g_wait_ns = 0;
Clock::time_point g_stats_t0 = Clock::now();
u64 g_stats_busy0 = 0;

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield");
#endif
}
inline u64 now_ns() {
    return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// The fences make "publish progress, then check whether the other side is
// asleep" and "announce sleeping, then re-check progress" a proper handshake
// (no store->load reordering), so no wake-up is lost.
void wake_consumer() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (g_consumer_sleeping.load()) {
        std::lock_guard lk(g_mu);
        g_cv_data.notify_one();
    }
}
void wake_producer() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (g_producer_waiting.load()) {
        std::lock_guard lk(g_mu);
        g_cv_progress.notify_one();
    }
}

// Producer side: wait (spin, then sleep) until pred() holds.
template <class P>
void wait_until(P pred) {
    if (pred()) return;
    u64 t0 = now_ns();
    for (int i = 0; i < 4000 && !pred(); i++) cpu_relax();
    while (!pred()) {
        std::unique_lock lk(g_mu);
        g_producer_waiting.store(true);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!pred()) g_cv_progress.wait_for(lk, std::chrono::milliseconds(2));
        g_producer_waiting.store(false);
    }
    g_wait_ns += now_ns() - t0;
}

void worker() {
    for (;;) {
        u64 r = g_read.load(std::memory_order_relaxed);
        u64 w = g_write.load(std::memory_order_acquire);
        if (r == w) {
            for (int i = 0; i < 2000 && g_write.load(std::memory_order_acquire) == r; i++) cpu_relax();
            if (g_write.load(std::memory_order_acquire) == r) {
                std::unique_lock lk(g_mu);
                g_consumer_sleeping.store(true);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (g_write.load() == r) g_cv_data.wait_for(lk, std::chrono::milliseconds(2));
                g_consumer_sleeping.store(false);
            }
            continue;
        }
        u64 t0 = now_ns();
        while (r != w) {
            size_t off = r % kRing;
            const Header* h = reinterpret_cast<const Header*>(g_ring + off);
            if (!h->fn) {
                r += kRing - off;
            } else {
                h->fn(g_ring + off + sizeof(Header));
                r += sizeof(Header) + align16(h->len);
            }
            g_read.store(r, std::memory_order_release);
            wake_producer();
            u64 t1 = now_ns();
            g_busy_ns.fetch_add(t1 - t0, std::memory_order_relaxed);
            t0 = t1;
            if (r == w) w = g_write.load(std::memory_order_acquire);  // keep going while there is more
        }
    }
}
}  // namespace

bool active() { return g_active; }

bool start(int core) {
    if (g_active) return true;
    g_ring = static_cast<u8*>(std::aligned_alloc(64, kRing));
    if (!g_ring) return false;
    std::atomic<int> state{0};  // 1 = running with the context, -1 = failed
    platform::gl_release_current();
    platform::ThreadHandle* th = platform::spawn_thread(
        [&state] {
            platform::set_thread_name("gl");
            platform::boost_current_thread();
            if (!platform::gl_make_current()) {
                state = -1;
                return;
            }
            state = 1;
            worker();
        },
        1 << 20, core);
    if (!th) {
        platform::gl_make_current();
        return false;
    }
    while (state.load() == 0) std::this_thread::yield();
    if (state.load() < 0) {
        platform::join_thread(th);
        platform::gl_make_current();
        LOGW("GL thread: could not make the context current there; GL stays on the main thread");
        return false;
    }
    platform::detach_thread(th);
    g_active = true;
    LOGI("GL thread started: the game's GL calls run on their own thread");
    return true;
}

void* begin(CmdFn fn, size_t len) {
    const u64 need = sizeof(Header) + align16(len);
    if (need > kRing / 2) fatal("GL queue: command of %zu bytes is too large", len);
    u64 pos = g_write.load(std::memory_order_relaxed);
    size_t off = pos % kRing;
    if (off + need > kRing) {
        // Not enough room before the end of the buffer: skip to its start.
        const u64 lap_start = pos - off;
        wait_until([&] { return g_read.load(std::memory_order_acquire) >= lap_start; });
        Header* h = reinterpret_cast<Header*>(g_ring + off);
        h->fn = nullptr;
        h->len = 0;
        pos += kRing - off;
        off = 0;
        g_write.store(pos);
        wake_consumer();
    }
    wait_until([&] { return pos + need - g_read.load(std::memory_order_acquire) <= kRing; });
    Header* h = reinterpret_cast<Header*>(g_ring + off);
    h->fn = fn;
    h->len = len;
    g_open_pos = pos;
    g_open_need = need;
    return g_ring + off + sizeof(Header);
}

void commit() {
    g_write.store(g_open_pos + g_open_need);
    wake_consumer();
}

void sync(const std::function<void()>& f) {
    if (!g_active) {
        f();
        return;
    }
    struct State {
        const std::function<void()>* f;
        std::atomic<bool> done;
    } st{&f, {false}};
    State* sp = &st;
    void* m = begin(
        [](const void* d) {
            State* s = *static_cast<State* const*>(d);
            (*s->f)();
            s->done.store(true);
            wake_producer();
        },
        sizeof sp);
    std::memcpy(m, &sp, sizeof sp);
    commit();
    wait_until([&] { return st.done.load(std::memory_order_acquire); });
}

void finish() {
    sync([] {});
}

bool (*g_idle_work)() = nullptr;
void set_idle_work(bool (*step)()) { g_idle_work = step; }

void swap(void (*do_swap)()) {
    if (!g_active) {
        do_swap();
        return;
    }
    // At most one frame in flight: the previous swap must be done. Until it
    // is, the idle work (the JIT cache translating ahead) may use the time.
    if (g_idle_work)
        while (g_swaps_done.load(std::memory_order_acquire) < g_swaps_queued && g_idle_work()) {
        }
    wait_until([] { return g_swaps_done.load(std::memory_order_acquire) >= g_swaps_queued; });
    void* m = begin(
        [](const void* d) {
            void (*fn)() = *static_cast<void (*const*)()>(d);
            u64 t0 = now_ns();
            fn();
            g_swap_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
            g_swaps_done.fetch_add(1);
            wake_producer();
        },
        sizeof do_swap);
    std::memcpy(m, &do_swap, sizeof do_swap);
    commit();
    g_swaps_queued++;
}

void stats(float& busy, float& waited_ms, float* swap_ms) {
    auto t = Clock::now();
    double span = std::chrono::duration<double, std::nano>(t - g_stats_t0).count();
    u64 b = g_busy_ns.load(), sw = g_swap_ns.load();
    double work = (double)(b - g_stats_busy0) - (double)(sw - g_stats_swap0);  // without the display wait
    busy = span > 0 ? (float)std::max(0.0, work / span) : 0.f;
    if (swap_ms) *swap_ms = (float)((sw - g_stats_swap0) / 1e6);
    g_stats_swap0 = sw;
    waited_ms = (float)(g_wait_ns / 1e6);
    g_wait_ns = 0;
    g_stats_busy0 = b;
    g_stats_t0 = t;
}

}  // namespace hle::glq
