#include "platform/thread.h"

#include <pthread.h>

#include <atomic>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <vector>

#include "core/common.h"
#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace platform {

struct ThreadHandle {
    pthread_t th;
    std::shared_ptr<std::atomic<bool>> finished;  // set when the thread's function has returned
};

namespace {
struct Start {
    std::function<void()> fn;
    int core;
    std::shared_ptr<std::atomic<bool>> finished;
};
std::atomic<int> g_next_core{1};
std::atomic<int> g_reserved_core{-1};

// Threads are released only by joining them. libnx has no pthread_detach (it
// returns ENOSYS and keeps the thread): a thread nobody joins keeps its kernel
// thread and its stack for good, and the Switch gives an application 96
// threads in all. The game starts a thread for each loading screen and for
// each car the garage shows, so until 0.3.1 the 97th thread could not start
// after an hour or so of play (the car stayed missing, the next race never
// loaded). Detached threads therefore wait here until they have finished, and
// the next spawn_thread or detach_thread joins them. The same on every
// platform, so the desktop build tests what the Switch runs.
std::mutex g_detached_mutex;
std::vector<ThreadHandle*> g_detached;
std::atomic<int> g_live{1};  // threads not yet released, the main thread included

// Desktop tests: NFSMW_THREAD_LIMIT=N refuses new threads past N (the Switch
// allows 96), NFSMW_NO_THREAD_REAP=1 never releases detached threads (as
// libnx's pthread_detach did before 0.3.1).
int test_limit() {
    static const int v = std::getenv("NFSMW_THREAD_LIMIT") ? std::atoi(std::getenv("NFSMW_THREAD_LIMIT")) : 0;
    return v;
}
bool no_reap() {
    static const bool v = std::getenv("NFSMW_NO_THREAD_REAP") != nullptr;
    return v;
}

void* trampoline(void* p) {
    Start* s = static_cast<Start*>(p);
#if defined(__SWITCH__)
    // Spread guest threads over cores 1 and 2; core 0 runs the game's main
    // (render) thread and core 3 belongs to the system.
    int core = s->core >= 0 ? s->core : 1 + (g_next_core.fetch_add(1) & 1);
    if (s->core < 0 && core == g_reserved_core.load()) core = core == 1 ? 2 : 1;
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
#endif
    s->fn();
    std::shared_ptr<std::atomic<bool>> finished = std::move(s->finished);
    delete s;
    finished->store(true, std::memory_order_release);
    return nullptr;
}

// Join the detached threads that have finished (each is at most a few
// instructions from its end, so the joins do not wait).
void reap() {
    std::vector<ThreadHandle*> done;
    {
        std::lock_guard lk(g_detached_mutex);
        for (size_t i = 0; i < g_detached.size();) {
            if (g_detached[i]->finished->load(std::memory_order_acquire)) {
                done.push_back(g_detached[i]);
                g_detached[i] = g_detached.back();
                g_detached.pop_back();
            } else {
                i++;
            }
        }
    }
    for (ThreadHandle* h : done) {
        pthread_join(h->th, nullptr);
        g_live.fetch_sub(1);
        delete h;
    }
}
}  // namespace

ThreadHandle* spawn_thread(std::function<void()> fn, size_t stack_size, int core) {
    reap();
    if (test_limit() > 0 && g_live.load() >= test_limit()) {
        LOGE("pthread_create failed: %d threads, the test limit (NFSMW_THREAD_LIMIT)", g_live.load());
        return nullptr;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_size);
    auto* h = new ThreadHandle;
    h->finished = std::make_shared<std::atomic<bool>>(false);
    auto* s = new Start{std::move(fn), core, h->finished};
    const int err = pthread_create(&h->th, &attr, trampoline, s);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        delete s;
        delete h;
        LOGE("pthread_create failed (error %d, %d threads running)", err, g_live.load());
        return nullptr;
    }
    g_live.fetch_add(1);
    return h;
}

void reserve_core_for_gl(int core) { g_reserved_core = core; }

void boost_current_thread() {
#if defined(__SWITCH__)
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);
#endif
}

unsigned long long thread_cpu_ns() {
#if defined(__SWITCH__)
    u64 ticks = 0;
    if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, (u64)-1))) return 0;
    return armTicksToNs(ticks);
#else
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
#endif
}

void set_thread_name(const char* name) {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}

void join_thread(ThreadHandle* t) {
    if (!t) return;
    pthread_join(t->th, nullptr);
    g_live.fetch_sub(1);
    delete t;
}

void detach_thread(ThreadHandle* t) {
    if (!t) return;
    if (no_reap()) return;  // test: the thread is never released
    {
        std::lock_guard lk(g_detached_mutex);
        g_detached.push_back(t);
    }
    reap();
}

int live_threads() { return g_live.load(); }

}  // namespace platform
