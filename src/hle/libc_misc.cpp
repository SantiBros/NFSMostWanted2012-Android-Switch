// Time, environment, process, dynamic linker, ARM runtime helpers, logging
// and the (absent) network stack.
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "core/elf.h"
#include "hle/hle.h"

namespace hle {

namespace {
using mem::ptr;
using mem::str;

// ---- time -------------------------------------------------------------------
s32 h_time(u32 t) {
    s32 now = (s32)std::time(nullptr);
    if (t) mem::w32(t, (u32)now);
    return now;
}
int h_gettimeofday(u32 tv, u32 tz) {
    struct timeval h;
    ::gettimeofday(&h, nullptr);
    if (tv) {
        mem::w32(tv, (u32)h.tv_sec);
        mem::w32(tv + 4, (u32)h.tv_usec);
    }
    if (tz) {
        mem::w32(tz, 0);
        mem::w32(tz + 4, 0);
    }
    return 0;
}
int h_clock_gettime(int clk, u32 ts) {
    static std::atomic<int> seen_mask{0};
    if (clk >= 0 && clk < 16 && !(seen_mask.fetch_or(1 << clk) & (1 << clk)))
        LOGI("clock_gettime: game uses clock id %d (lr %s)", clk, elf::describe(cpu::current().regs()[14]).c_str());
    struct timespec h;
    if (::clock_gettime(clk, &h) < 0) ::clock_gettime(CLOCK_MONOTONIC, &h);
    mem::w32(ts, (u32)h.tv_sec);
    mem::w32(ts + 4, (u32)h.tv_nsec);
    return 0;
}
s32 h_clock() {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 3) LOGI("clock() called from %s", elf::describe(cpu::current().regs()[14]).c_str());
    return (s32)std::clock();
}
int h_nanosleep(u32 req, u32 rem) {
    s64 ns = (s64)(s32)mem::r32(req) * 1000000000ll + (s32)mem::r32(req + 4);
    if (ns > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    if (rem) {
        mem::w32(rem, 0);
        mem::w32(rem + 4, 0);
    }
    return 0;
}
int h_usleep(u32 us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
    return 0;
}
int h_sched_yield() {
    std::this_thread::yield();
    return 0;
}

// bionic LP32 struct tm: 9 ints, long tm_gmtoff, const char* tm_zone
void put_tm(u32 g, const struct tm& t) {
    const int f[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    for (int i = 0; i < 9; i++) mem::w32(g + i * 4, (u32)f[i]);
#if defined(__SWITCH__)
    mem::w32(g + 36, 0);
    mem::w32(g + 40, intern_string("UTC"));
#else
    mem::w32(g + 36, (u32)t.tm_gmtoff);
    mem::w32(g + 40, intern_string(t.tm_zone ? t.tm_zone : "UTC"));
#endif
}
struct tm get_tm(u32 g) {
    struct tm t {};
    int* f[9] = {&t.tm_sec, &t.tm_min, &t.tm_hour, &t.tm_mday, &t.tm_mon, &t.tm_year, &t.tm_wday, &t.tm_yday, &t.tm_isdst};
    for (int i = 0; i < 9; i++) *f[i] = (int)mem::r32(g + i * 4);
    return t;
}
thread_local u32 t_tm_buf = 0;
u32 tm_buf() {
    if (!t_tm_buf) t_tm_buf = mem::heap_calloc(1, 44);
    return t_tm_buf;
}
u32 h_localtime(u32 tp) {
    time_t t = (s32)mem::r32(tp);
    struct tm r;
    ::localtime_r(&t, &r);
    put_tm(tm_buf(), r);
    return tm_buf();
}
u32 h_gmtime(u32 tp) {
    time_t t = (s32)mem::r32(tp);
    struct tm r;
    ::gmtime_r(&t, &r);
    put_tm(tm_buf(), r);
    return tm_buf();
}
u32 h_gmtime_r(u32 tp, u32 out) {
    time_t t = (s32)mem::r32(tp);
    struct tm r;
    ::gmtime_r(&t, &r);
    put_tm(out, r);
    return out;
}
u32 h_strftime(u32 buf, u32 max, u32 fmt, u32 tmp) {
    struct tm t = get_tm(tmp);
    return (u32)std::strftime(ptr<char>(buf), max, str(fmt), &t);
}
void h_tzset() {}

// ---- environment --------------------------------------------------------------
std::mutex g_env_mutex;
std::map<std::string, std::string> g_env = {
    {"ANDROID_DATA", "/data"},
    {"ANDROID_ROOT", "/system"},
    {"EXTERNAL_STORAGE", "/sdcard"},
    {"HOME", "/data"},
    {"TMPDIR", "/data/local/tmp"},
};
u32 h_getenv(u32 name) {
    std::lock_guard lk(g_env_mutex);
    auto it = g_env.find(str(name));
    if (it == g_env.end()) return 0;
    return intern_string(it->second);
}
int h_setenv(u32 name, u32 value, int overwrite) {
    std::lock_guard lk(g_env_mutex);
    if (!overwrite && g_env.count(str(name))) return 0;
    g_env[str(name)] = str(value);
    return 0;
}
int h_unsetenv(u32 name) {
    std::lock_guard lk(g_env_mutex);
    g_env.erase(str(name));
    return 0;
}

// ---- random -------------------------------------------------------------------
std::mutex g_rand_mutex;
u64 g_rand48 = 0x1234ABCD330Eull;
void h_srand48(s32 seed) {
    std::lock_guard lk(g_rand_mutex);
    g_rand48 = ((u64)(u32)seed << 16) | 0x330E;
}
s32 h_lrand48() {
    std::lock_guard lk(g_rand_mutex);
    g_rand48 = (g_rand48 * 0x5DEECE66Dull + 0xB) & 0xFFFFFFFFFFFFull;
    return (s32)(g_rand48 >> 17);
}

// ---- process ------------------------------------------------------------------
struct AtExit {
    u32 fn, arg, dso;
};
std::mutex g_atexit_mutex;
std::vector<AtExit> g_atexit;

int h_cxa_atexit(u32 fn, u32 arg, u32 dso) {
    std::lock_guard lk(g_atexit_mutex);
    g_atexit.push_back({fn, arg, dso});
    return 0;
}
int h_aeabi_atexit(u32 arg, u32 fn, u32 dso) { return h_cxa_atexit(fn, arg, dso); }
void run_atexit(u32 dso) {
    for (;;) {
        AtExit e;
        {
            std::lock_guard lk(g_atexit_mutex);
            auto it = std::find_if(g_atexit.rbegin(), g_atexit.rend(),
                                   [dso](const AtExit& a) { return dso == 0 || a.dso == dso; });
            if (it == g_atexit.rend()) return;
            e = *it;
            g_atexit.erase(std::next(it).base());
        }
        cpu::current().call(e.fn, {e.arg});
    }
}
void h_cxa_finalize(u32 dso) { run_atexit(dso); }
int h_cxa_thread_atexit_impl(u32 fn, u32 obj, u32 dso) {
    (void)dso;
    // Per-thread destructors run when the guest thread exits.
    add_thread_dtor(fn, obj);
    return 0;
}
void h_exit(int code) { guest_exit(code); }
void h_abort() {
    cpu::crash_report(cpu::current(), "abort() called");
    fatal("guest called abort()");
}
void h_stack_chk_fail() {
    cpu::crash_report(cpu::current(), "stack smashing detected");
    fatal("__stack_chk_fail");
}
void h_assert2(u32 file, int line, u32 func, u32 msg) {
    cpu::crash_report(cpu::current(), "assertion failed");
    fatal("assert: %s:%d %s: %s", str(file), line, str(func), str(msg));
}
u32 h_errno() { return cpu::current().errno_addr(); }
s32 h_getpid() { return 4242; }
u32 h_getuid() { return 10123; }
int h_getpwuid_r(u32, u32, u32, u32, u32 result) {
    mem::w32(result, 0);
    return 2;
}
int h_gethostname(u32 buf, u32 n) {
    std::snprintf(ptr<char>(buf), n, "localhost");
    return 0;
}
int h_prctl(int option, u32 a2, u32, u32, u32) {
    if (option == 15 && a2) LOGI("thread %u named \"%s\"", cpu::current().id, str(a2));  // PR_SET_NAME
    return 0;
}
int h_fail_minus1() {
    cpu::current().set_errno(38);  // ENOSYS
    return -1;
}
int h_zero() { return 0; }
int h_sigaction(int, u32, u32 old) {
    if (old) std::memset(mem::host(old), 0, 16);
    return 0;
}
int h_raise(int sig) {
    LOGW("raise(%d) ignored", sig);
    return 0;
}
int h_kill(int pid, int sig) {
    LOGW("kill(%d, %d) ignored", pid, sig);
    return 0;
}
int h_system(u32 cmd) {
    LOGW("system(\"%s\") refused", cmd ? str(cmd) : "");
    return -1;
}

// sysconf() constants are bionic's, not glibc's.
s32 h_sysconf(int name) {
    switch (name) {
    case 0x06: return 100;          // _SC_CLK_TCK
    case 0x0b: return 1024;         // _SC_OPEN_MAX
    case 0x27: case 0x28: return 4096;  // _SC_PAGESIZE / _SC_PAGE_SIZE
    case 0x60: case 0x61: return 4;     // _SC_NPROCESSORS_CONF / ONLN
    case 0x62: return (2u << 30) / 4096;  // _SC_PHYS_PAGES (2 GiB)
    case 0x63: return (1u << 30) / 4096;  // _SC_AVPHYS_PAGES
    default: LOGD("sysconf(0x%x) unknown", name); return -1;
    }
}

u32 h_getauxval(u32 type) {
    if (type == 16) {  // AT_HWCAP: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32
        return (1 << 1) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 12) | (1 << 13) | (1 << 15) |
               (1 << 16) | (1 << 17) | (1 << 18) | (1 << 19);
    }
    return 0;
}

// raw syscall() with ARM EABI numbers
void h_syscall(cpu::GuestThread& t) {
    auto& r = t.regs();
    u32 nr = r[0];
    switch (nr) {
    case 224:  // gettid
        r[0] = t.id;
        return;
    case 158:  // sched_yield
        std::this_thread::yield();
        r[0] = 0;
        return;
    case 240: {  // futex(uaddr, op, val, timeout...)
        r[0] = (u32)futex_op(r[1], (int)r[2], r[3], mem::r32(r[13]));
        return;
    }
    default:
        LOGW("syscall(%u) not supported", nr);
        t.set_errno(38);
        r[0] = (u32)-1;
    }
}

// ---- ARM run-time ABI helpers (normally from libgcc) --------------------------
s32 h_idiv(s32 a, s32 b) { return b ? (a == INT32_MIN && b == -1 ? a : a / b) : 0; }
u32 h_uidiv(u32 a, u32 b) { return b ? a / b : 0; }
void h_idivmod(cpu::GuestThread& t) {
    auto& r = t.regs();
    s32 a = (s32)r[0], b = (s32)r[1];
    if (!b) { r[0] = r[1] = 0; return; }
    if (a == INT32_MIN && b == -1) { r[0] = (u32)a; r[1] = 0; return; }
    r[0] = (u32)(a / b);
    r[1] = (u32)(a % b);
}
void h_uidivmod(cpu::GuestThread& t) {
    auto& r = t.regs();
    u32 a = r[0], b = r[1];
    if (!b) { r[0] = r[1] = 0; return; }
    r[0] = a / b;
    r[1] = a % b;
}
void h_uldivmod(cpu::GuestThread& t) {
    auto& r = t.regs();
    u64 a = r[0] | ((u64)r[1] << 32), b = r[2] | ((u64)r[3] << 32);
    u64 q = b ? a / b : 0, m = b ? a % b : 0;
    r[0] = (u32)q; r[1] = (u32)(q >> 32); r[2] = (u32)m; r[3] = (u32)(m >> 32);
}
void h_ldivmod(cpu::GuestThread& t) {
    auto& r = t.regs();
    s64 a = (s64)(r[0] | ((u64)r[1] << 32)), b = (s64)(r[2] | ((u64)r[3] << 32));
    s64 q = b ? a / b : 0, m = b ? a % b : 0;
    r[0] = (u32)q; r[1] = (u32)((u64)q >> 32); r[2] = (u32)m; r[3] = (u32)((u64)m >> 32);
}
double h_dadd(double a, double b) { return a + b; }
double h_dmul(double a, double b) { return a * b; }
int h_dcmplt(double a, double b) { return a < b; }
double h_f2d(float a) { return a; }
double h_ui2d(u32 a) { return a; }
double h_ul2d(u64 a) { return (double)a; }

// ---- C++ exception support ----------------------------------------------------
u32 h_unwind_find_exidx(u32 pc, u32 pcount) {
    elf::Module* m = elf::module_for(pc);
    if (!m || !m->exidx) {
        mem::w32(pcount, 0);
        return 0;
    }
    mem::w32(pcount, m->exidx_count);
    return m->exidx;
}

// ---- dynamic linker -----------------------------------------------------------
constexpr u32 HOST_LIB_HANDLE = 0x7F000000;
constexpr u32 RTLD_DEFAULT_LP32 = 0xFFFFFFFF;
thread_local std::string t_dlerror;
u32 h_dlopen(u32 name, int flags) {
    (void)flags;
    if (!name) return RTLD_DEFAULT_LP32 - 2;  // the "main program" handle
    std::string n = str(name);
    std::string base = n.substr(n.find_last_of('/') + 1);
    auto& mods = elf::all();
    for (size_t i = 0; i < mods.size(); i++)
        if (mods[i]->name == base) return 0x7E000000 + (u32)i;
    if (base == "libOpenSLES.so") {
        if (!opensl_available()) {
            t_dlerror = "dlopen failed: library \"" + n + "\" not found";
            LOGI("dlopen(%s) -> not available", n.c_str());
            return 0;
        }
    }
    static const char* host_libs[] = {"libc.so", "libm.so", "libdl.so", "liblog.so", "libEGL.so", "libGLESv1_CM.so",
                                      "libGLESv2.so", "libGLESv3.so", "libandroid.so", "libOpenSLES.so",
                                      "libjnigraphics.so", "libstdc++.so"};
    for (const char* h : host_libs)
        if (base == h) {
            LOGD("dlopen(%s) -> host library", n.c_str());
            return HOST_LIB_HANDLE;
        }
    t_dlerror = "dlopen failed: library \"" + n + "\" not found";
    LOGW("dlopen(%s) failed", n.c_str());
    return 0;
}
u32 h_dlsym(u32 handle, u32 name) {
    std::string n = str(name);
    u32 a = 0;
    if (handle >= 0x7E000000 && handle < 0x7F000000) {
        auto& mods = elf::all();
        u32 i = handle - 0x7E000000;
        if (i < mods.size()) a = mods[i]->find_export(n);
        if (!a) a = elf::lookup_global(n);
    } else {
        a = elf::lookup_global(n);
    }
    if (!a) {
        t_dlerror = "undefined symbol: " + n;
        LOGD("dlsym(%s) -> not found", n.c_str());
    }
    return a;
}
int h_dlclose(u32) { return 0; }
u32 h_dlerror() {
    if (t_dlerror.empty()) return 0;
    u32 a = intern_string(t_dlerror);
    t_dlerror.clear();
    return a;
}
int h_dladdr(u32 addr, u32 info) {
    elf::Module* m = elf::module_for(addr);
    if (!m) return 0;
    mem::w32(info + 0, intern_string(m->name));
    mem::w32(info + 4, m->base);
    u32 a = addr & ~1u;
    auto it = m->by_addr.upper_bound(a);
    if (it != m->by_addr.begin()) {
        --it;
        mem::w32(info + 8, intern_string(it->second));
        mem::w32(info + 12, it->first);
    } else {
        mem::w32(info + 8, 0);
        mem::w32(info + 12, 0);
    }
    return 1;
}

// ---- sorting --------------------------------------------------------------------
void h_qsort(u32 base, u32 n, u32 size, u32 cmp) {
    if (n < 2 || size == 0) return;
    auto& t = cpu::current();
    std::vector<u32> idx(n);
    for (u32 i = 0; i < n; i++) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](u32 a, u32 b) {
        return (s32)t.call(cmp, {base + a * size, base + b * size}) < 0;
    });
    std::vector<u8> tmp((size_t)n * size);
    for (u32 i = 0; i < n; i++) std::memcpy(&tmp[(size_t)i * size], mem::host(base + idx[i] * size), size);
    std::memcpy(mem::host(base), tmp.data(), tmp.size());
}

// ---- Android logging --------------------------------------------------------------
logging::Level log_level(int prio) {
    switch (prio) {
    case 2: return logging::Verbose;
    case 3: return logging::Debug;
    case 4: return logging::Info;
    case 5: return logging::Warn;
    default: return prio >= 6 ? logging::Error : logging::Debug;
    }
}
int h_log_print(int prio, u32 tag, u32 fmt, abi::VarArgs va) {
    std::string s = format(str(fmt), va);
    while (!s.empty() && s.back() == '\n') s.pop_back();
    logging::write(log_level(prio), tag ? str(tag) : "?", "%s", s.c_str());
    return 1;
}
int h_log_write(int prio, u32 tag, u32 text) {
    std::string s = text ? str(text) : "";
    while (!s.empty() && s.back() == '\n') s.pop_back();
    logging::write(log_level(prio), tag ? str(tag) : "?", "%s", s.c_str());
    return 1;
}
void h_log_assert(u32 cond, u32 tag, u32 fmt, abi::VarArgs va) {
    std::string s = fmt ? format(str(fmt), va) : "";
    cpu::crash_report(cpu::current(), "__android_log_assert");
    fatal("log_assert [%s] %s: %s", tag ? str(tag) : "", cond ? str(cond) : "", s.c_str());
}

}  // namespace

[[noreturn]] void guest_exit(int code) {
    LOGI("guest exit(%d)", code);
    if (cpu::current_or_null()) run_atexit(0);
    std::fflush(nullptr);
    std::_Exit(code);
}

void register_libc_misc() {
    REG("time", h_time);
    REG("gettimeofday", h_gettimeofday);
    REG("clock_gettime", h_clock_gettime);
    REG("clock", h_clock);
    REG("nanosleep", h_nanosleep);
    REG("usleep", h_usleep);
    REG("sched_yield", h_sched_yield);
    REG("localtime", h_localtime);
    REG("gmtime", h_gmtime);
    REG("gmtime_r", h_gmtime_r);
    REG("strftime", h_strftime);
    REG("tzset", h_tzset);
    {
        u32 tzname = mem::static_alloc(8, 4);
        mem::w32(tzname, intern_string("UTC"));
        mem::w32(tzname + 4, intern_string("UTC"));
        thunks::add_data("tzname", tzname);
        thunks::add_data("timezone", mem::static_alloc(4, 4));
    }
    REG("getenv", h_getenv);
    REG("setenv", h_setenv);
    REG("unsetenv", h_unsetenv);
    REG("srand48", h_srand48);
    REG("lrand48", h_lrand48);
    REG_RE("__cxa_atexit", h_cxa_atexit);
    REG_RE("__aeabi_atexit", h_aeabi_atexit);
    REG_RE("__cxa_finalize", h_cxa_finalize);
    REG("__cxa_thread_atexit_impl", h_cxa_thread_atexit_impl);
    REG_RE("exit", h_exit);
    REG("abort", h_abort);
    REG("__stack_chk_fail", h_stack_chk_fail);
    {
        u32 guard = mem::static_alloc(4, 4);
        mem::w32(guard, 0xA5C3F00Du ^ (u32)std::random_device{}());
        thunks::add_data("__stack_chk_guard", guard);
        thunks::add_data("__dso_handle", mem::static_alloc(4, 4));
    }
    REG("__assert2", h_assert2);
    REG("__errno", h_errno);
    REG("getpid", h_getpid);
    for (const char* n : {"getuid", "geteuid", "getgid", "getegid"}) REG(n, h_getuid);
    REG("getpwuid_r", h_getpwuid_r);
    REG("gethostname", h_gethostname);
    REG("prctl", h_prctl);
    for (const char* n : {"fork", "execl", "execv", "waitpid"}) REG(n, h_fail_minus1);
    REG("sigaction", h_sigaction);
    REG("sigprocmask", h_zero);
    REG("raise", h_raise);
    REG("kill", h_kill);
    REG("system", h_system);
    REG("sysconf", h_sysconf);
    REG("getauxval", h_getauxval);
    REG_RAW("syscall", h_syscall);
    REG("__aeabi_idiv", h_idiv);
    REG("__aeabi_uidiv", h_uidiv);
    REG_RAW("__aeabi_idivmod", h_idivmod);
    REG_RAW("__aeabi_uidivmod", h_uidivmod);
    REG_RAW("__aeabi_uldivmod", h_uldivmod);
    REG_RAW("__aeabi_ldivmod", h_ldivmod);
    REG("__aeabi_dadd", h_dadd);
    REG("__aeabi_dmul", h_dmul);
    REG("__aeabi_dcmplt", h_dcmplt);
    REG("__aeabi_f2d", h_f2d);
    REG("__aeabi_ui2d", h_ui2d);
    REG("__aeabi_ul2d", h_ul2d);
    REG("__gnu_Unwind_Find_exidx", h_unwind_find_exidx);
    REG("dlopen", h_dlopen);
    REG("dlsym", h_dlsym);
    REG("dlclose", h_dlclose);
    REG("dlerror", h_dlerror);
    REG("dladdr", h_dladdr);
    REG_RE("qsort", h_qsort);
    REG("__android_log_print", h_log_print);
    REG("__android_log_write", h_log_write);
    REG("__android_log_assert", h_log_assert);
    // setjmp family lives in the ARM runtime blob
    for (const char* n : {"setjmp", "_setjmp", "sigsetjmp"}) thunks::add_data(n, mem::RT_SETJMP);
    for (const char* n : {"longjmp", "_longjmp", "siglongjmp"}) thunks::add_data(n, mem::RT_LONGJMP);
}

}  // namespace hle
