// pthreads and semaphores. Guest sync objects (4-byte bionic structs) are
// mapped by address to host objects created on first use, so statically
// initialised mutexes (PTHREAD_MUTEX_INITIALIZER) just work.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/elf.h"
#include "hle/hle.h"
#include "platform/thread.h"

namespace hle {

namespace {
using namespace std::chrono;

constexpr int EBUSY_ = 16, ETIMEDOUT_ = 110, EINVAL_ = 22, EAGAIN_ = 11;

template <class T>
class SyncTable {
public:
    T& get(u32 addr) {
        {
            std::shared_lock lk(m_);
            auto it = map_.find(addr);
            if (it != map_.end()) return *it->second;
        }
        std::unique_lock lk(m_);
        auto& p = map_[addr];
        if (!p) p = std::make_unique<T>(addr);
        return *p;
    }
    void reset(u32 addr) {
        std::unique_lock lk(m_);
        map_.erase(addr);
    }

private:
    std::shared_mutex m_;
    std::unordered_map<u32, std::unique_ptr<T>> map_;
};

// ---- mutex ------------------------------------------------------------------
struct GMutex {
    explicit GMutex(u32 addr) {
        u32 v = mem::r32(addr);
        recursive = (v & 0xC000) == 0x4000;
    }
    std::recursive_timed_mutex m;
    bool recursive;
    std::atomic<std::thread::id> owner{};
    int depth = 0;
};
SyncTable<GMutex> g_mutexes;

int h_mutex_init(u32 m, u32 attr) {
    u32 type = attr ? mem::r32(attr) & 3 : 0;
    mem::w32(m, type == 1 ? 0x4000 : type == 2 ? 0x8000 : 0);
    g_mutexes.reset(m);
    return 0;
}
int h_mutex_destroy(u32 m) {
    g_mutexes.reset(m);
    return 0;
}
int h_mutex_lock(u32 m) {
    if (!m) return EINVAL_;
    GMutex& g = g_mutexes.get(m);
    g.m.lock();
    g.depth++;
    g.owner = std::this_thread::get_id();
    return 0;
}
int h_mutex_trylock(u32 m) {
    GMutex& g = g_mutexes.get(m);
    if (!g.recursive && g.owner.load() == std::this_thread::get_id()) return EBUSY_;
    if (!g.m.try_lock()) return EBUSY_;
    g.depth++;
    g.owner = std::this_thread::get_id();
    return 0;
}
int h_mutex_unlock(u32 m) {
    if (!m) return EINVAL_;
    GMutex& g = g_mutexes.get(m);
    if (g.owner.load() != std::this_thread::get_id()) return 1;  // EPERM
    if (--g.depth == 0) g.owner = std::thread::id{};
    g.m.unlock();
    return 0;
}
int h_mutexattr_init(u32 a) { mem::w32(a, 0); return 0; }
int h_mutexattr_settype(u32 a, int type) { mem::w32(a, (u32)type); return 0; }
int h_ok() { return 0; }

// ---- condition variables -------------------------------------------------------
struct GCond {
    explicit GCond(u32) {}
    std::condition_variable_any cv;
};
SyncTable<GCond> g_conds;

// Wait with the guest mutex fully released and re-acquired with the same depth.
template <class Wait>
int cond_wait_impl(u32 c, u32 m, Wait wait) {
    GMutex& gm = g_mutexes.get(m);
    GCond& gc = g_conds.get(c);
    int depth = gm.depth;
    gm.depth = 0;
    gm.owner = std::thread::id{};
    for (int i = 1; i < depth; i++) gm.m.unlock();
    // cv waits on the mutex: unlocks the last level, re-locks before returning
    bool ok = wait(gc.cv, gm.m);
    for (int i = 1; i < depth; i++) gm.m.lock();
    gm.depth = depth;
    gm.owner = std::this_thread::get_id();
    return ok ? 0 : ETIMEDOUT_;
}
int h_cond_wait(u32 c, u32 m) {
    LOGV("cond_wait(c=%08X, m=%08X) thread %u", c, m, cpu::current().id);
    return cond_wait_impl(c, m, [](std::condition_variable_any& cv, std::recursive_timed_mutex& mx) {
        std::unique_lock<std::recursive_timed_mutex> lk(mx, std::adopt_lock);
        cv.wait(lk);
        lk.release();
        return true;
    });
}
int h_cond_timedwait(u32 c, u32 m, u32 abstime) {
    s64 sec = (s32)mem::r32(abstime), nsec = (s32)mem::r32(abstime + 4);
    auto deadline = system_clock::time_point(seconds(sec) + nanoseconds(nsec));
    return cond_wait_impl(c, m, [deadline](std::condition_variable_any& cv, std::recursive_timed_mutex& mx) {
        std::unique_lock<std::recursive_timed_mutex> lk(mx, std::adopt_lock);
        bool r = cv.wait_until(lk, deadline) == std::cv_status::no_timeout;
        lk.release();
        return r;
    });
}
int h_cond_signal(u32 c) {
    LOGV("cond_signal(c=%08X) thread %u", c, cpu::current().id);
    g_conds.get(c).cv.notify_one();
    return 0;
}
int h_cond_broadcast(u32 c) {
    LOGV("cond_broadcast(c=%08X) thread %u", c, cpu::current().id);
    g_conds.get(c).cv.notify_all();
    return 0;
}
int h_cond_destroy(u32 c) { LOGV("cond_destroy(%08X)", c); g_conds.reset(c); return 0; }
int h_cond_init(u32 c, u32) { LOGV("cond_init(%08X)", c); g_conds.reset(c); return 0; }

// ---- rwlock ------------------------------------------------------------------
struct GRw {
    explicit GRw(u32) {}
    std::mutex m;
    std::condition_variable cv;
    int readers = 0;
    bool writer = false;
};
SyncTable<GRw> g_rws;
int h_rw_rdlock(u32 l) {
    GRw& g = g_rws.get(l);
    std::unique_lock lk(g.m);
    g.cv.wait(lk, [&] { return !g.writer; });
    g.readers++;
    return 0;
}
int h_rw_wrlock(u32 l) {
    GRw& g = g_rws.get(l);
    std::unique_lock lk(g.m);
    g.cv.wait(lk, [&] { return !g.writer && g.readers == 0; });
    g.writer = true;
    return 0;
}
int h_rw_unlock(u32 l) {
    GRw& g = g_rws.get(l);
    std::unique_lock lk(g.m);
    if (g.writer) g.writer = false;
    else if (g.readers > 0) g.readers--;
    g.cv.notify_all();
    return 0;
}
int h_rw_init(u32 l, u32) { g_rws.reset(l); return 0; }

// ---- semaphores ------------------------------------------------------------
struct GSem {
    explicit GSem(u32 addr) : count(mem::r32(addr)) {}
    std::mutex m;
    std::condition_variable cv;
    u32 count;
};
SyncTable<GSem> g_sems;
int h_sem_init(u32 s, int, u32 value) {
    mem::w32(s, value);
    g_sems.reset(s);
    g_sems.get(s);
    return 0;
}
int h_sem_destroy(u32 s) { g_sems.reset(s); return 0; }
int h_sem_post(u32 s) {
    GSem& g = g_sems.get(s);
    std::lock_guard lk(g.m);
    g.count++;
    g.cv.notify_one();
    return 0;
}
int h_sem_wait(u32 s) {
    GSem& g = g_sems.get(s);
    std::unique_lock lk(g.m);
    g.cv.wait(lk, [&] { return g.count > 0; });
    g.count--;
    return 0;
}
int h_sem_trywait(u32 s) {
    GSem& g = g_sems.get(s);
    std::lock_guard lk(g.m);
    if (!g.count) {
        cpu::current().set_errno(EAGAIN_);
        return -1;
    }
    g.count--;
    return 0;
}
int h_sem_timedwait(u32 s, u32 abstime) {
    GSem& g = g_sems.get(s);
    s64 sec = (s32)mem::r32(abstime), nsec = (s32)mem::r32(abstime + 4);
    auto deadline = system_clock::time_point(seconds(sec) + nanoseconds(nsec));
    std::unique_lock lk(g.m);
    if (!g.cv.wait_until(lk, deadline, [&] { return g.count > 0; })) {
        cpu::current().set_errno(ETIMEDOUT_);
        return -1;
    }
    g.count--;
    return 0;
}
int h_sem_getvalue(u32 s, u32 out) {
    GSem& g = g_sems.get(s);
    std::lock_guard lk(g.m);
    mem::w32(out, g.count);
    return 0;
}

// ---- once / keys ----------------------------------------------------------------
std::recursive_mutex g_once_mutex;
int h_once(u32 once, u32 fn) {
    if (mem::r32(once) == 2) return 0;
    std::lock_guard lk(g_once_mutex);
    if (mem::r32(once) != 0) return 0;
    mem::w32(once, 1);
    cpu::current().call(fn, {});
    mem::w32(once, 2);
    return 0;
}
std::mutex g_keys_mutex;
std::vector<u32> g_key_dtors(128, 0);
std::vector<bool> g_key_used(128, false);
int h_key_create(u32 key, u32 dtor) {
    std::lock_guard lk(g_keys_mutex);
    for (u32 i = 0; i < 128; i++)
        if (!g_key_used[i]) {
            g_key_used[i] = true;
            g_key_dtors[i] = dtor;
            mem::w32(key, i);
            return 0;
        }
    return EAGAIN_;
}
int h_key_delete(u32 k) {
    std::lock_guard lk(g_keys_mutex);
    if (k < 128) g_key_used[k] = false;
    return 0;
}
u32 h_getspecific(u32 k) { return k < 128 ? cpu::current().key_values[k] : 0; }
int h_setspecific(u32 k, u32 v) {
    if (k >= 128) return EINVAL_;
    cpu::current().key_values[k] = v;
    return 0;
}

// ---- threads -------------------------------------------------------------------------
// bionic LP32 pthread_attr_t: flags, stack_base, stack_size, guard_size, policy, priority
constexpr u32 ATTR_DETACHED = 1;
constexpr u32 DEFAULT_STACK = 1024 * 1024;

struct ThreadRec {
    u32 id = 0;
    platform::ThreadHandle* th = nullptr;
    u32 retval = 0;
    bool detached = false;
    u32 stack_base = 0, stack_size = 0;
};
std::mutex g_threads_mutex;
std::unordered_map<u32, std::shared_ptr<ThreadRec>> g_threads;
std::atomic<u32> g_next_id{2000};

thread_local std::vector<std::pair<u32, u32>> t_dtors;

void run_thread_cleanup(cpu::GuestThread& t) {
    // __cxa_thread_atexit destructors, newest first
    while (!t_dtors.empty()) {
        auto d = t_dtors.back();
        t_dtors.pop_back();
        t.call(d.first, {d.second});
    }
    // pthread key destructors (up to 4 rounds like bionic)
    for (int round = 0; round < 4; round++) {
        bool any = false;
        for (u32 k = 0; k < 128; k++) {
            u32 v = t.key_values[k];
            u32 dtor;
            {
                std::lock_guard lk(g_keys_mutex);
                dtor = g_key_used[k] ? g_key_dtors[k] : 0;
            }
            if (v && dtor) {
                t.key_values[k] = 0;
                t.call(dtor, {v});
                any = true;
            }
        }
        if (!any) break;
    }
}

int h_attr_init(u32 a) {
    std::memset(mem::host(a), 0, 24);
    mem::w32(a + 8, DEFAULT_STACK);
    mem::w32(a + 12, 4096);
    return 0;
}
int h_attr_setdetachstate(u32 a, int st) {
    u32 f = mem::r32(a);
    mem::w32(a, st ? (f | ATTR_DETACHED) : (f & ~ATTR_DETACHED));
    return 0;
}
int h_attr_setstacksize(u32 a, u32 sz) { mem::w32(a + 8, sz); return 0; }
int h_attr_setstack(u32 a, u32 base, u32 sz) { mem::w32(a + 4, base); mem::w32(a + 8, sz); return 0; }
int h_attr_getstack(u32 a, u32 base, u32 sz) {
    mem::w32(base, mem::r32(a + 4));
    mem::w32(sz, mem::r32(a + 8));
    return 0;
}
int h_attr_setschedparam(u32 a, u32 param) { mem::w32(a + 20, param ? mem::r32(param) : 0); return 0; }
int h_attr_setschedpolicy(u32 a, int p) { mem::w32(a + 16, (u32)p); return 0; }

int h_create(u32 out, u32 attr, u32 start, u32 arg) {
    auto rec = std::make_shared<ThreadRec>();
    rec->id = g_next_id.fetch_add(1);
    u32 stack_size = DEFAULT_STACK, stack_base = 0;
    if (attr) {
        rec->detached = mem::r32(attr) & ATTR_DETACHED;
        stack_base = mem::r32(attr + 4);
        stack_size = mem::r32(attr + 8);
        if (stack_size < 64 * 1024) stack_size = 64 * 1024;
    }
    // Translated code needs a little more stack headroom than native code would.
    if (!stack_base) stack_size = std::max<u32>(stack_size, 256 * 1024);
    {
        std::lock_guard lk(g_threads_mutex);
        g_threads[rec->id] = rec;
    }
    mem::w32(out, rec->id);
    u32 id = rec->id;
    LOGD("pthread_create -> thread %u, entry %s", id, elf::describe(start).c_str());
    rec->th = platform::spawn_thread([rec, id, start, arg, stack_size, stack_base] {
        cpu::GuestThread* t = cpu::attach_current_thread(stack_size, stack_base, id);
        rec->stack_base = t->stack_base;
        rec->stack_size = t->stack_size;
        u32 ret = 0;
        try {
            ret = (u32)t->call(start, {arg});
        } catch (const cpu::ThreadExit& e) {
            ret = e.value;
        }
        try {
            run_thread_cleanup(*t);
        } catch (const cpu::ThreadExit&) {
        }
        rec->retval = ret;
        cpu::detach_current_thread();
        LOGD("thread %u finished", id);
    }, 1 << 20);
    if (!rec->th) return EAGAIN_;
    if (rec->detached) {
        platform::detach_thread(rec->th);
        rec->th = nullptr;
    }
    return 0;
}
int h_join(u32 id, u32 retval) {
    std::shared_ptr<ThreadRec> rec;
    {
        std::lock_guard lk(g_threads_mutex);
        auto it = g_threads.find(id);
        if (it == g_threads.end()) return 3;  // ESRCH
        rec = it->second;
    }
    if (rec->th) {
        platform::join_thread(rec->th);
        rec->th = nullptr;
    }
    if (retval) mem::w32(retval, rec->retval);
    std::lock_guard lk(g_threads_mutex);
    g_threads.erase(id);
    return 0;
}
int h_detach(u32 id) {
    std::lock_guard lk(g_threads_mutex);
    auto it = g_threads.find(id);
    if (it == g_threads.end()) return 3;
    if (it->second->th) {
        platform::detach_thread(it->second->th);
        it->second->th = nullptr;
    }
    it->second->detached = true;
    return 0;
}
u32 h_self() { return cpu::current().id; }
int h_equal(u32 a, u32 b) { return a == b; }
void h_exit(u32 v) { throw cpu::ThreadExit{v}; }
int h_getattr_np(u32 id, u32 attr) {
    h_attr_init(attr);
    auto& t = cpu::current();
    if (id == t.id) {
        mem::w32(attr + 4, t.stack_base);
        mem::w32(attr + 8, t.stack_size);
    }
    return 0;
}
int h_getschedparam(u32, u32 policy, u32 param) {
    if (policy) mem::w32(policy, 0);
    if (param) mem::w32(param, 0);
    return 0;
}

// ---- raw futex (rarely used directly) ---------------------------------------------
std::mutex g_futex_mutex;
std::condition_variable g_futex_cv;

}  // namespace

void add_thread_dtor(u32 fn, u32 obj) { t_dtors.emplace_back(fn, obj); }

s32 futex_op(u32 uaddr, int op, u32 val, u32 timeout) {
    int cmd = op & 0x7F;
    if (cmd == 0) {  // FUTEX_WAIT
        std::unique_lock lk(g_futex_mutex);
        if (mem::r32(uaddr) != val) return -11;
        if (timeout) {
            s64 ns = (s64)(s32)mem::r32(timeout) * 1000000000ll + (s32)mem::r32(timeout + 4);
            g_futex_cv.wait_for(lk, nanoseconds(ns));
        } else {
            g_futex_cv.wait_for(lk, milliseconds(50));
        }
        return 0;
    }
    if (cmd == 1) {  // FUTEX_WAKE
        g_futex_cv.notify_all();
        return (s32)val;
    }
    return -38;
}

void register_pthread() {
    REG("pthread_mutex_init", h_mutex_init);
    REG("pthread_mutex_destroy", h_mutex_destroy);
    REG("pthread_mutex_lock", h_mutex_lock);
    REG("pthread_mutex_trylock", h_mutex_trylock);
    REG("pthread_mutex_unlock", h_mutex_unlock);
    REG("pthread_mutexattr_init", h_mutexattr_init);
    REG("pthread_mutexattr_settype", h_mutexattr_settype);
    REG("pthread_mutexattr_destroy", h_ok);
    REG("pthread_mutexattr_setpshared", h_ok);
    REG("pthread_cond_init", h_cond_init);
    REG("pthread_cond_destroy", h_cond_destroy);
    REG("pthread_cond_wait", h_cond_wait);
    REG("pthread_cond_timedwait", h_cond_timedwait);
    REG("pthread_cond_signal", h_cond_signal);
    REG("pthread_cond_broadcast", h_cond_broadcast);
    REG("pthread_rwlock_init", h_rw_init);
    REG("pthread_rwlock_destroy", h_ok);
    REG("pthread_rwlock_rdlock", h_rw_rdlock);
    REG("pthread_rwlock_wrlock", h_rw_wrlock);
    REG("pthread_rwlock_unlock", h_rw_unlock);
    REG("sem_init", h_sem_init);
    REG("sem_destroy", h_sem_destroy);
    REG("sem_post", h_sem_post);
    REG("sem_wait", h_sem_wait);
    REG("sem_trywait", h_sem_trywait);
    REG("sem_timedwait", h_sem_timedwait);
    REG("sem_getvalue", h_sem_getvalue);
    REG_RE("pthread_once", h_once);
    REG("pthread_key_create", h_key_create);
    REG("pthread_key_delete", h_key_delete);
    REG("pthread_getspecific", h_getspecific);
    REG("pthread_setspecific", h_setspecific);
    REG("pthread_attr_init", h_attr_init);
    REG("pthread_attr_destroy", h_ok);
    REG("pthread_attr_setdetachstate", h_attr_setdetachstate);
    REG("pthread_attr_setstacksize", h_attr_setstacksize);
    REG("pthread_attr_setstack", h_attr_setstack);
    REG("pthread_attr_getstack", h_attr_getstack);
    REG("pthread_attr_setschedparam", h_attr_setschedparam);
    REG("pthread_attr_setschedpolicy", h_attr_setschedpolicy);
    REG("pthread_create", h_create);
    REG("pthread_join", h_join);
    REG("pthread_detach", h_detach);
    REG("pthread_self", h_self);
    REG("pthread_equal", h_equal);
    REG_RE("pthread_exit", h_exit);
    REG("pthread_getattr_np", h_getattr_np);
    REG("pthread_getschedparam", h_getschedparam);
    REG("pthread_setschedparam", h_ok);
}

}  // namespace hle
