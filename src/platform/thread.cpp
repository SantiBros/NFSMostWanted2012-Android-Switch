#include "platform/thread.h"

#include <pthread.h>

#include <atomic>

#include "core/common.h"
#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace platform {

struct ThreadHandle {
    pthread_t th;
};

namespace {
struct Start {
    std::function<void()> fn;
    int core;
};
std::atomic<int> g_next_core{1};
std::atomic<int> g_reserved_core{-1};

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
    delete s;
    return nullptr;
}
}  // namespace

ThreadHandle* spawn_thread(std::function<void()> fn, size_t stack_size, int core) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_size);
    auto* h = new ThreadHandle;
    auto* s = new Start{std::move(fn), core};
    if (pthread_create(&h->th, &attr, trampoline, s) != 0) {
        pthread_attr_destroy(&attr);
        delete s;
        delete h;
        LOGE("pthread_create failed");
        return nullptr;
    }
    pthread_attr_destroy(&attr);
    return h;
}

void reserve_core_for_gl(int core) { g_reserved_core = core; }

void boost_current_thread() {
#if defined(__SWITCH__)
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);
#endif
}

void join_thread(ThreadHandle* t) {
    if (!t) return;
    pthread_join(t->th, nullptr);
    delete t;
}

void detach_thread(ThreadHandle* t) {
    if (!t) return;
    pthread_detach(t->th);
    delete t;
}

}  // namespace platform
