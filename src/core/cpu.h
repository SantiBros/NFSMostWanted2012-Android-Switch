// One GuestThread per host thread that runs guest (32-bit ARM) code.
// Each owns a Dynarmic JIT instance; all share the guest memory and a
// global exclusive monitor so LDREX/STREX work across threads.
#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <initializer_list>
#include <memory>

#include "core/common.h"

namespace Dynarmic::A32 {
class Jit;
}

namespace cpu {

struct Callbacks;

// Thrown (from a re-entrant thunk) to unwind a guest thread, e.g. pthread_exit.
struct ThreadExit {
    u32 value;
};

struct GuestThread {
    u32 id = 0;           // guest pthread_t / tid
    u32 tls_base = 0;     // 64 bionic TLS slots
    u32 tpidruro = 0;     // CP15 c13 read-only thread register (= tls_base)
    u32 tpidrurw = 0;
    u32 stack_base = 0, stack_size = 0;
    bool owns_stack = false;
    u32 pending_svc = 0;
    bool has_pending = false;
    int call_depth = 0;
    size_t processor_id = 0;
    std::array<u32, 128> key_values{};  // pthread_getspecific storage
    // Diagnostics: the host function this thread is currently inside (if any).
    const char* volatile in_thunk = nullptr;
    volatile u32 in_thunk_lr = 0;
    u64 thunk_calls = 0;
    u64 guest_insns = 0;  // with NFSMW_COUNT_INSNS=1
    u64 slow_reads = 0;   // memory reads through the callbacks (exclusive loads, page-table misses)
    u64 excl_writes = 0;  // exclusive stores (STREX) that reached memory
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    std::unique_ptr<Callbacks> callbacks;
    bool is_main = false;
    u64 jit_blocks = 0;                // blocks this JIT has translated (over every thread that used it)
    u64 compile_blocks_at_attach = 0;  // the host thread's translation count when it attached
    // A spare JIT that the JIT cache translates ahead into (core/jit_cache.h)
    // while it waits in the pool; taking it stops that.
    bool warming = false;
    std::atomic<bool> warm_stop{false};
    std::mutex warm_mutex;  // held by the JIT cache for each block it translates

    std::array<u32, 16>& regs();
    u32 errno_addr() const { return tls_base + 2 * 4; }  // TLS_SLOT_ERRNO
    void set_errno(int e);

    // Call a guest function (bit 0 set = Thumb) with up to N word arguments
    // following the soft-float AAPCS. Returns r0 | r1 << 32.
    u64 call(u32 func, const u32* args, size_t n);
    u64 call(u32 func, std::initializer_list<u32> args) { return call(func, args.begin(), args.size()); }

    ~GuestThread();
};

// direct_memory: on platforms without fault handling (Switch), let the JIT
// access guest memory directly instead of through the page table. Faster, but
// an access outside guest memory is no longer caught (the game makes none).
void init(bool direct_memory = false);
// Create the guest context for the calling host thread.
// entry: the guest function the thread starts in (0 = not known).
GuestThread* attach_current_thread(u32 stack_size, u32 existing_stack = 0, u32 id = 0, u32 entry = 0);
void detach_current_thread();
// A JIT of its own, for no thread yet (core/jit_cache.h translates ahead into
// it), and handing it to the pool of finished threads' JITs that new guest
// threads take over. expected_blocks ranks it among them.
GuestThread* create_spare(size_t code_cache_size);
void pool_add_warming(GuestThread* t, u64 expected_blocks);
GuestThread& current();
GuestThread* current_or_null();

// Register dump + backtrace guess, written to the log.
void crash_report(GuestThread& t, const char* reason);
// Log what every guest thread is doing (which host call it is blocked in).
void dump_threads();
// Count host calls made by the calling thread between start/stop (debugging).
void trace_calls_begin();
void trace_calls_end(const char* label);
void note_call(GuestThread& t, const char* name);
void trace_all(bool on);
// Time spent in each host function on the main thread (debugging).
void profile_begin();
void profile_end(int frames, double tick_ms_total);  // histogram of host calls by thread (excluding the main thread)

}  // namespace cpu
