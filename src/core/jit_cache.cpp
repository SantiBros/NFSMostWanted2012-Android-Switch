#include "core/jit_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/compile_stats.h>

#include "core/cpu.h"
#include "core/elf.h"
#include "core/memory.h"
#include "platform/thread.h"

#ifndef NFSMW_VERSION
#define NFSMW_VERSION "dev"
#endif

namespace jit_cache {

namespace {
using Clock = std::chrono::steady_clock;

constexpr char kMagic[8] = {'N', 'F', 'S', 'M', 'W', 'J', 'I', 'T'};
constexpr u32 kFormat = 1;
// Space a JIT keeps free when translating ahead (a full code cache is thrown
// away as a whole, which would undo the work).
constexpr size_t kReserve = 16u << 20;
#if defined(__aarch64__)
constexpr size_t kSpareCache = 64u << 20;  // the loading threads' JIT (others have 32 MiB)
#else
constexpr size_t kSpareCache = 128u << 20;
#endif
constexpr auto kSaveEvery = std::chrono::seconds(20);

struct List {
    const char* file;
    std::mutex mutex;
    std::vector<u64> order;  // every block known, in the order first seen
    std::unordered_set<u64> known;
    size_t loaded = 0;  // of those, from the file
    size_t saved = 0;   // order.size() at the last save
    explicit List(const char* f) : file(f) {}
};
List g_main("jit-main.bin"), g_workers("jit-workers.bin");
std::vector<u64> g_main_warm, g_worker_warm;  // the lists as loaded (read-only after init)
size_t g_main_next = 0;
std::atomic<u64> g_main_warmed{0}, g_worker_warmed{0};
std::string g_dir;
u64 g_identity = 0;
bool g_on = false;
std::mutex g_save_mutex;
struct Range {
    u32 lo, hi;
};
std::vector<Range> g_code;  // the game libraries' code (only blocks there are listed)

bool in_code(u32 pc) {
    for (const Range& r : g_code)
        if (pc >= r.lo && pc < r.hi) return true;
    return false;
}

// Dynarmic calls this after translating each block.
void on_compile(u64 descriptor) {
    const cpu::GuestThread* t = cpu::current_or_null();
    if (!t) return;  // the warm-up thread: already listed
    if (!in_code((u32)descriptor)) return;
    List& l = t->is_main ? g_main : g_workers;
    std::lock_guard lk(l.mutex);
    if (l.known.insert(descriptor).second) l.order.push_back(descriptor);
}

// FNV-1a over the port version and the game libraries' code: lists from a
// different game build or port version are not used.
u64 compute_identity() {
    u64 h = 0xCBF29CE484222325ull;
    auto add = [&h](const u8* p, size_t n) {
        for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    };
    const std::string v = NFSMW_VERSION;
    add(reinterpret_cast<const u8*>(v.data()), v.size());
    for (const elf::Module* m : elf::all()) {
        add(reinterpret_cast<const u8*>(m->name.data()), m->name.size());
        if (m->exec_hi > m->exec_lo) add(mem::host(m->exec_lo), m->exec_hi - m->exec_lo);
    }
    return h;
}

std::string path_of(const List& l) { return g_dir + "/" + l.file; }

void load(List& l, std::vector<u64>& warm) {
    FILE* f = std::fopen(path_of(l).c_str(), "rb");
    if (!f) return;
    char magic[8];
    u32 format = 0, count = 0;
    u64 identity = 0;
    bool ok = std::fread(magic, 1, 8, f) == 8 && std::equal(magic, magic + 8, kMagic) &&
              std::fread(&format, 4, 1, f) == 1 && std::fread(&count, 4, 1, f) == 1 && std::fread(&identity, 8, 1, f) == 1 &&
              format == kFormat && identity == g_identity && count < (1u << 24);
    std::vector<u64> d;
    if (ok) {
        d.resize(count);
        ok = std::fread(d.data(), 8, count, f) == count;
    }
    std::fclose(f);
    if (!ok) {
        LOGI("JIT cache: %s is from another game build or port version; starting a new one", l.file);
        return;
    }
    std::lock_guard lk(l.mutex);
    for (u64 x : d)
        if (in_code((u32)x) && l.known.insert(x).second) l.order.push_back(x);
    l.loaded = l.saved = l.order.size();
    warm = l.order;
}

void write(List& l) {
    std::vector<u64> d;
    {
        std::lock_guard lk(l.mutex);
        if (l.order.size() == l.saved) return;
        d = l.order;
    }
    const std::string path = path_of(l), tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    const u32 format = kFormat, count = (u32)d.size();
    bool ok = std::fwrite(kMagic, 1, 8, f) == 8 && std::fwrite(&format, 4, 1, f) == 1 && std::fwrite(&count, 4, 1, f) == 1 &&
              std::fwrite(&g_identity, 8, 1, f) == 1 && std::fwrite(d.data(), 8, d.size(), f) == d.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok) {
        std::remove(tmp.c_str());
        return;
    }
    std::remove(path.c_str());  // the Switch's rename does not replace
    if (std::rename(tmp.c_str(), path.c_str()) != 0) return;
    std::lock_guard lk(l.mutex);
    l.saved = std::max(l.saved, d.size());
}

void saver_thread() {
    platform::set_thread_name("jit-cache");
    for (;;) {
        std::this_thread::sleep_for(kSaveEvery);
        save();
    }
}

// The spare JIT for loading threads, and when it may be handed out: the game
// starts its long-lived service threads (controller, sound streaming) during
// start-up; the spare is kept for the loading threads that come later.
std::atomic<bool> g_spare_may_go{false};
}  // namespace

void fingerprint() { g_identity = compute_identity(); }

void init(const std::string& dir) {
    g_dir = dir;
    for (const elf::Module* m : elf::all())
        if (m->exec_hi > m->exec_lo) g_code.push_back({m->exec_lo, m->exec_hi});
    if (!g_identity) g_identity = compute_identity();
    g_on = true;
    load(g_main, g_main_warm);
    load(g_workers, g_worker_warm);
    Dynarmic::g_compile_hook = &on_compile;
    LOGI("JIT cache: %zu main-thread and %zu loading-thread blocks known from earlier sessions", g_main_warm.size(),
         g_worker_warm.size());
    platform::detach_thread(platform::spawn_thread(saver_thread, 128 * 1024));
}

void start_worker_warmup() {
    if (!g_on || g_worker_warm.empty()) return;
    platform::detach_thread(platform::spawn_thread(
        [] {
            platform::set_thread_name("jit-warm");
            const auto t0 = Clock::now();
            cpu::GuestThread* spare = cpu::create_spare(kSpareCache);
            if (!spare) {
                LOGW("JIT cache: not enough memory for a spare JIT; loading threads translate as they go");
                return;
            }
            bool in_pool = false, stopped = false;
            u64 n = 0;
            for (u64 d : g_worker_warm) {
                if (!in_pool && g_spare_may_go.load()) {
                    cpu::pool_add_warming(spare, g_worker_warm.size());
                    in_pool = true;
                }
                std::lock_guard wl(spare->warm_mutex);
                if (spare->warm_stop.load()) {
                    stopped = true;
                    break;
                }
                if (spare->jit->CodeCacheRemaining() < kReserve) break;
                spare->jit->PrecompileBlock(d);
                n++;
                g_worker_warmed = n;
            }
            const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
            LOGI("JIT cache: %llu of %zu loading-thread blocks translated ahead in %.1f s%s", (unsigned long long)n,
                 g_worker_warm.size(), secs, stopped ? " (then a loading thread took the JIT over)" : "");
            if (!in_pool) {
                while (!g_spare_may_go.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
                cpu::pool_add_warming(spare, g_worker_warm.size());
            }
        },
        256 * 1024));
}

void spare_may_go() { g_spare_may_go = true; }

bool main_idle_step() {
    if (!g_on || g_main_next >= g_main_warm.size()) return false;
    cpu::GuestThread* t = cpu::current_or_null();
    if (!t || !t->is_main || t->call_depth != 0) return false;
    if (t->jit->CodeCacheRemaining() < kReserve) {
        g_main_next = g_main_warm.size();
        return false;
    }
    const u64 before = Dynarmic::t_compile_stats.blocks;
    t->jit->PrecompileBlock(g_main_warm[g_main_next++]);
    if (Dynarmic::t_compile_stats.blocks != before) g_main_warmed++;
    return true;
}

void save() {
    if (!g_on) return;
    std::lock_guard lk(g_save_mutex);
    write(g_main);
    write(g_workers);
}

std::string summary() {
    if (!g_on) return "";
    size_t main_known, workers_known;
    {
        std::lock_guard lk(g_main.mutex);
        main_known = g_main.order.size();
    }
    {
        std::lock_guard lk(g_workers.mutex);
        workers_known = g_workers.order.size();
    }
    char out[200];
    std::snprintf(out, sizeof out, "JIT cache: %zu + %zu blocks listed, %llu + %llu translated ahead", main_known,
                  workers_known, (unsigned long long)g_main_warmed.load(), (unsigned long long)g_worker_warmed.load());
    return out;
}

}  // namespace jit_cache
