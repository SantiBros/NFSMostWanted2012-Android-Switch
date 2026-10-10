#include "core/cpu.h"

#include <algorithm>
#include <cstring>
#include <atomic>
#include <map>
#include <string>
#include <chrono>
#include <mutex>
#include <vector>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include "core/elf.h"
#include "core/memory.h"
#include "core/thunks.h"
#include "hle/hle.h"

namespace cpu {

using namespace Dynarmic;

namespace {

constexpr size_t MAX_PROCESSORS = 256;
ExclusiveMonitor* g_monitor = nullptr;
std::mutex g_pid_mutex;
std::vector<bool> g_pid_used(MAX_PROCESSORS, false);
std::atomic<u32> g_next_tid{1000};
thread_local GuestThread* t_current = nullptr;
std::mutex g_threads_mutex;
std::vector<GuestThread*> g_threads;
thread_local bool t_tracing = false;
thread_local std::map<std::string, int>* t_trace = nullptr;
// Global (all threads) call histogram, enabled with NFSMW_TRACE_ALL=1.
std::atomic<bool> g_trace_all{false};
std::mutex g_trace_all_mutex;
std::map<std::string, int> g_trace_all_counts;
// Host-call profile of the main thread (debug command "profile").
std::atomic<bool> g_prof{false};
constexpr size_t kProfSlots = 4096;
u64 g_prof_ns[kProfSlots], g_prof_n[kProfSlots];
inline u64 prof_now() {
    return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

size_t alloc_processor_id() {
    std::lock_guard lk(g_pid_mutex);
    for (size_t i = 0; i < MAX_PROCESSORS; i++)
        if (!g_pid_used[i]) {
            g_pid_used[i] = true;
            return i;
        }
    fatal("too many guest threads");
}
void free_processor_id(size_t id) {
    std::lock_guard lk(g_pid_mutex);
    g_pid_used[id] = false;
}

// CP15: thread ID registers (bionic reads TLS with mrc p15,0,rX,c13,c0,3)
// and the legacy barrier operations (mcr p15,0,rX,c7,c10,{4,5}).
// Generic timer ticks at 19.2 MHz like most ARM SoCs.
std::uint64_t timer_ticks(void*, std::uint32_t, std::uint32_t) {
    using namespace std::chrono;
    auto ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    return (std::uint64_t)((__int128)ns * 192 / 10000);
}
std::uint64_t timer_freq(void*, std::uint32_t, std::uint32_t) { return 19200000; }
std::uint64_t noop(void*, std::uint32_t, std::uint32_t) { return 0; }

// Any coprocessor operation we do not model: log once at translation time and
// treat it as a no-op that reads zero (instead of aborting inside Dynarmic).
class GenericCoproc : public A32::Coprocessor {
public:
    explicit GenericCoproc(int num) : num_(num) {}
    std::optional<Callback> CompileInternalOperation(bool, unsigned opc1, A32::CoprocReg CRd, A32::CoprocReg CRn,
                                                     A32::CoprocReg CRm, unsigned opc2) override {
        warn("CDP", opc1, (int)CRd, (int)CRn, (int)CRm, opc2);
        return Callback{&noop, std::nullopt};
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm,
                                               unsigned opc2) override {
        warn("MCR", opc1, -1, (int)CRn, (int)CRm, opc2);
        return Callback{&noop, std::nullopt};
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned opc, A32::CoprocReg CRm) override {
        warn("MCRR", opc, -1, -1, (int)CRm, 0);
        return Callback{&noop, std::nullopt};
    }
    CallbackOrAccessOneWord CompileGetOneWord(bool, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm,
                                              unsigned opc2) override {
        warn("MRC", opc1, -1, (int)CRn, (int)CRm, opc2);
        return Callback{&noop, std::nullopt};
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned opc, A32::CoprocReg CRm) override {
        warn("MRRC", opc, -1, -1, (int)CRm, 0);
        return Callback{&noop, std::nullopt};
    }
    std::optional<Callback> CompileLoadWords(bool, bool, A32::CoprocReg, std::optional<std::uint8_t>) override {
        warn("LDC", 0, -1, -1, -1, 0);
        return Callback{&noop, std::nullopt};
    }
    std::optional<Callback> CompileStoreWords(bool, bool, A32::CoprocReg, std::optional<std::uint8_t>) override {
        warn("STC", 0, -1, -1, -1, 0);
        return Callback{&noop, std::nullopt};
    }

protected:
    void warn(const char* op, unsigned opc1, int crd, int crn, int crm, unsigned opc2) {
        LOGW("unhandled coprocessor op %s p%d, opc1=%u CRd=%d CRn=c%d CRm=c%d opc2=%u (treated as no-op)", op, num_,
             opc1, crd, crn, crm, opc2);
    }
    int num_;
};

// CP15: thread ID registers (bionic reads TLS with mrc p15,0,rX,c13,c0,3),
// legacy barriers (mcr p15,0,rX,c7,...), and the generic timer.
class CP15 final : public GenericCoproc {
public:
    explicit CP15(GuestThread* t) : GenericCoproc(15), t_(t) {}
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, A32::CoprocReg CRn,
                                               A32::CoprocReg CRm, unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C7) {
            return Callback{&noop, std::nullopt};  // barriers / cache maintenance
        }
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C13 && CRm == A32::CoprocReg::C0 && opc2 == 2) {
            return &t_->tpidrurw;
        }
        return GenericCoproc::CompileSendOneWord(two, opc1, CRn, CRm, opc2);
    }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, A32::CoprocReg CRn,
                                              A32::CoprocReg CRm, unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C13 && CRm == A32::CoprocReg::C0) {
            if (opc2 == 3) return &t_->tpidruro;
            if (opc2 == 2) return &t_->tpidrurw;
        }
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C14 && CRm == A32::CoprocReg::C0 && opc2 == 0) {
            return Callback{&timer_freq, std::nullopt};  // CNTFRQ
        }
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C9 && CRm == A32::CoprocReg::C13 && opc2 == 0) {
            return Callback{&timer_ticks, std::nullopt};  // PMCCNTR (cycle counter)
        }
        return GenericCoproc::CompileGetOneWord(two, opc1, CRn, CRm, opc2);
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, A32::CoprocReg CRm) override {
        if (!two && CRm == A32::CoprocReg::C14 && (opc == 0 || opc == 1)) {
            return Callback{&timer_ticks, std::nullopt};  // CNTPCT / CNTVCT
        }
        return GenericCoproc::CompileGetTwoWords(two, opc, CRm);
    }

private:
    GuestThread* t_;
};

}  // namespace

struct Callbacks final : public A32::UserCallbacks {
    GuestThread* t = nullptr;
    int bad_reads = 0;

    void bad_access(u32 a, int size, bool write) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "invalid %s of %d bytes at 0x%08X", write ? "write" : "read", size, a);
        crash_report(*t, buf);
        if (write || ++bad_reads > 8) fatal("guest memory fault: %s", buf);
    }
    bool ok(u32 a) const { return a >= mem::NULL_GUARD_END && a < mem::g_size; }

    std::uint8_t MemoryRead8(A32::VAddr a) override {
        t->slow_reads++;
        if (!ok(a)) { bad_access(a, 1, false); return 0; }
        return mem::r8(a);
    }
    std::uint16_t MemoryRead16(A32::VAddr a) override {
        t->slow_reads++;
        if (!ok(a)) { bad_access(a, 2, false); return 0; }
        return mem::r16(a);
    }
    std::uint32_t MemoryRead32(A32::VAddr a) override {
        t->slow_reads++;
        if (!ok(a)) { bad_access(a, 4, false); return 0; }
        return mem::r32(a);
    }
    std::uint64_t MemoryRead64(A32::VAddr a) override {
        t->slow_reads++;
        if (!ok(a)) { bad_access(a, 8, false); return 0; }
        return mem::r64(a);
    }
    std::optional<std::uint32_t> MemoryReadCode(A32::VAddr a) override {
        if (!ok(a)) return std::nullopt;
        return mem::r32(a);
    }
    void MemoryWrite8(A32::VAddr a, std::uint8_t v) override {
        if (!ok(a)) return bad_access(a, 1, true);
        mem::w8(a, v);
    }
    void MemoryWrite16(A32::VAddr a, std::uint16_t v) override {
        if (!ok(a)) return bad_access(a, 2, true);
        mem::w16(a, v);
    }
    void MemoryWrite32(A32::VAddr a, std::uint32_t v) override {
        if (!ok(a)) return bad_access(a, 4, true);
        mem::w32(a, v);
    }
    void MemoryWrite64(A32::VAddr a, std::uint64_t v) override {
        if (!ok(a)) return bad_access(a, 8, true);
        mem::w64(a, v);
    }
    template <class T>
    bool cas(A32::VAddr a, T v, T expected) {
        t->excl_writes++;
        if (!ok(a)) {
            bad_access(a, sizeof(T), true);
            return false;
        }
        return __atomic_compare_exchange_n(reinterpret_cast<T*>(mem::host(a)), &expected, v, false,
                                           __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    bool MemoryWriteExclusive8(A32::VAddr a, std::uint8_t v, std::uint8_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive16(A32::VAddr a, std::uint16_t v, std::uint16_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive32(A32::VAddr a, std::uint32_t v, std::uint32_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive64(A32::VAddr a, std::uint64_t v, std::uint64_t e) override { return cas(a, v, e); }

    void InterpreterFallback(A32::VAddr pc, size_t) override {
        t->regs()[15] = pc;
        crash_report(*t, "interpreter fallback requested");
        fatal("unsupported instruction at 0x%08X", pc);
    }

    void CallSVC(std::uint32_t swi) override {
        if (swi == mem::SVC_RETURN_TRAP || swi == mem::SVC_THREAD_EXIT) {
            t->pending_svc = swi;
            t->has_pending = true;
            t->jit->HaltExecution(HaltReason::UserDefined2);
            return;
        }
        const thunks::Entry* e = thunks::entry(swi);
        if (!e) {
            crash_report(*t, "unknown SVC");
            fatal("unknown SVC 0x%X", swi);
        }
        if (e->reentrant) {
            t->pending_svc = swi;
            t->has_pending = true;
            t->jit->HaltExecution(HaltReason::UserDefined1);
            return;
        }
        t->thunk_calls++;
        if (t_tracing || g_trace_all.load(std::memory_order_relaxed)) note_call(*t, e->name.c_str());
        t->in_thunk = e->name.c_str();
        t->in_thunk_lr = t->regs()[14];
        if (g_prof.load(std::memory_order_relaxed) && t->id == 1000 && swi < kProfSlots) {
            u64 t0 = prof_now();
            e->fn(*t);
            g_prof_ns[swi] += prof_now() - t0;
            g_prof_n[swi]++;
        } else {
            e->fn(*t);
        }
        t->in_thunk = nullptr;
    }

    void ExceptionRaised(A32::VAddr pc, A32::Exception ex) override {
        switch (ex) {
        case A32::Exception::WaitForInterrupt:
        case A32::Exception::WaitForEvent:
        case A32::Exception::SendEvent:
        case A32::Exception::SendEventLocal:
        case A32::Exception::Yield:
        case A32::Exception::PreloadData:
        case A32::Exception::PreloadDataWithIntentToWrite:
        case A32::Exception::PreloadInstruction:
            return;
        default:
            break;
        }
        t->regs()[15] = pc;
        char buf[96];
        std::snprintf(buf, sizeof buf, "CPU exception %d at 0x%08X", (int)ex, pc);
        crash_report(*t, buf);
        fatal("%s", buf);
    }

    // Only called with NFSMW_COUNT_INSNS=1 (cycle counting = guest instructions).
    void AddTicks(std::uint64_t n) override { t->guest_insns += n; }
    std::uint64_t GetTicksRemaining() override { return 1ull << 40; }
};

std::array<u32, 16>& GuestThread::regs() { return jit->Regs(); }

void GuestThread::set_errno(int e) { mem::w32(errno_addr(), (u32)e); }

GuestThread::~GuestThread() = default;

u64 GuestThread::call(u32 func, const u32* args, size_t n) {
    auto& r = jit->Regs();
    const std::array<u32, 16> saved = r;
    const std::array<u32, 64> saved_ext = jit->ExtRegs();
    const u32 saved_cpsr = jit->Cpsr();
    const u32 saved_fpscr = jit->Fpscr();
    const bool saved_pending = has_pending;
    const u32 saved_svc = pending_svc;

    u32 sp = call_depth == 0 ? stack_base + stack_size : r[13];
    sp -= 32;
    size_t nstack = n > 4 ? n - 4 : 0;
    sp = (sp - (u32)nstack * 4) & ~15u;
    for (size_t i = 4; i < n; i++) mem::w32(sp + (u32)(i - 4) * 4, args[i]);
    for (size_t i = 0; i < 4; i++) r[i] = i < n ? args[i] : 0;
    r[13] = sp;
    r[14] = mem::RT_RETURN_TRAP;
    r[15] = func & ~1u;
    jit->SetCpsr((func & 1) ? 0x30 : 0x10);  // user mode, Thumb bit from target

    call_depth++;
    for (;;) {
        has_pending = false;
        jit->Run();
        if (!has_pending) continue;  // halted for another reason (e.g. external request)
        if (pending_svc == mem::SVC_RETURN_TRAP) break;
        if (pending_svc == mem::SVC_THREAD_EXIT) {
            call_depth--;
            throw ThreadExit{r[0]};
        }
        thunks::dispatch(*this, pending_svc);
    }
    call_depth--;

    u64 ret = (u64)r[0] | ((u64)r[1] << 32);
    r = saved;
    jit->ExtRegs() = saved_ext;
    jit->SetCpsr(saved_cpsr);
    jit->SetFpscr(saved_fpscr);
    has_pending = saved_pending;
    pending_svc = saved_svc;
    return ret;
}

// Without fastmem: one page-table entry per 4 KiB guest page. In "absolute
// offset" mode Dynarmic adds the full guest address to the entry, so every
// mapped page simply holds the window base; unmapped pages are null and fall
// back to the memory callbacks (which report the bad access).
std::array<std::uint8_t*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>* g_page_table = nullptr;

bool g_use_page_table = false;
bool g_unchecked_fastmem = false;

void init(bool direct_memory) {
    g_monitor = new ExclusiveMonitor(MAX_PROCESSORS);
    // NFSMW_PAGE_TABLE=1 (debugging): use the page table even where fastmem
    // is available, to compare the two memory access models.
    const bool force_pt = std::getenv("NFSMW_PAGE_TABLE") != nullptr;
    g_use_page_table = !mem::g_fastmem || force_pt;
    // Switch: no fault handling, but every valid guest address lies inside the
    // window, so the JIT can still add the window base directly. The page
    // table stays as the fallback for accesses the JIT does not inline.
    g_unchecked_fastmem = !mem::g_fastmem && direct_memory && !force_pt;
    LOGI("memory access: %s", !g_use_page_table ? "fastmem" : g_unchecked_fastmem ? "direct (unchecked fastmem)" : "page table");
    if (g_use_page_table) {
        if (mem::g_fastmem) LOGW("memory access: page table (NFSMW_PAGE_TABLE)");
        g_page_table = new std::array<std::uint8_t*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>{};
        u64 pages = mem::g_size >> 12;
        for (u64 i = mem::NULL_GUARD_END >> 12; i < pages && i < g_page_table->size(); i++) (*g_page_table)[i] = mem::g_base;
    }
}

GuestThread* attach_current_thread(u32 stack_size, u32 existing_stack, u32 id) {
    if (t_current) return t_current;
    auto* t = new GuestThread;
    const bool is_main = (g_next_tid.load() == 1000 && id == 0);
    t->id = id ? id : g_next_tid.fetch_add(1);
    t->processor_id = alloc_processor_id();
    t->callbacks = std::make_unique<Callbacks>();
    t->callbacks->t = t;

    // bionic TLS block: slot 0 = self, slot 1 = thread id, slot 2 = errno
    t->tls_base = mem::heap_calloc(64, 4);
    mem::w32(t->tls_base + 0, t->tls_base);
    mem::w32(t->tls_base + 4, t->id);
    t->tpidruro = t->tls_base;

    if (existing_stack) {
        t->stack_base = existing_stack;
        t->stack_size = stack_size;
    } else {
        t->stack_size = stack_size;
        t->stack_base = mem::heap_alloc(stack_size, 4096);
        t->owns_stack = true;
        if (!t->stack_base) fatal("cannot allocate %u byte guest stack", stack_size);
    }

    A32::UserConfig cfg;
    cfg.callbacks = t->callbacks.get();
    cfg.processor_id = t->processor_id;
    cfg.global_monitor = g_monitor;
    cfg.arch_version = A32::ArchVersion::v7;
    if (!g_use_page_table) {
        cfg.fastmem_pointer = reinterpret_cast<uintptr_t>(mem::g_base);
        cfg.recompile_on_fastmem_failure = true;
        cfg.fastmem_exclusive_access = true;
        cfg.recompile_on_exclusive_fastmem_failure = true;
    } else {
        cfg.page_table = g_page_table;
        cfg.absolute_offset_page_table = true;
        if (g_unchecked_fastmem) {
            cfg.fastmem_pointer = reinterpret_cast<uintptr_t>(mem::g_base);
            cfg.fastmem_without_fault_handler = true;
            cfg.recompile_on_fastmem_failure = false;
        }
    }
    // NFSMW_COUNT_INSNS=1 (debugging): count executed guest instructions.
    static const bool count_insns = std::getenv("NFSMW_COUNT_INSNS") != nullptr;
    cfg.enable_cycle_counting = count_insns;
    cfg.always_little_endian = true;
    // NFSMW_UNSAFE_FP=1 (debugging, x86 only): skip the exact-NaN/rounding
    // emulation of ARM floating point, to see how much of the game code's time
    // on x86 goes to it. (The ARM64 backend runs ARM floating point natively.)
    static const bool unsafe_fp = std::getenv("NFSMW_UNSAFE_FP") != nullptr;
    if (unsafe_fp) {
        cfg.unsafe_optimizations = true;
        cfg.optimizations |= OptimizationFlag::Unsafe_UnfuseFMA | OptimizationFlag::Unsafe_InaccurateNaN |
                             OptimizationFlag::Unsafe_ReducedErrorFP | OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
    }
    // None of the game's libraries reads the cumulative floating-point exception
    // flags (no VMRS of FPSCR into a register anywhere in their code), so the
    // JIT need not track them. On the ARM64 backend (Switch) that saves a write
    // and a read of the FP status register around the floating-point code of
    // every block. NFSMW_FP_FLAGS=1 tracks them again (for comparisons).
    static const bool fp_flags = std::getenv("NFSMW_FP_FLAGS") != nullptr;
    cfg.ignore_fp_exception_flags = !fp_flags;
    cfg.define_unpredictable_behaviour = true;
    for (int i = 0; i < 15; i++)
        if (i != 10 && i != 11) cfg.coprocessors[i] = std::make_shared<GenericCoproc>(i);
    cfg.coprocessors[15] = std::make_shared<CP15>(t);
#if defined(__aarch64__)
    cfg.code_cache_size = is_main ? 128u * 1024 * 1024 : 32u * 1024 * 1024;  // arm64 backend limit: 128 MiB
#else
    cfg.code_cache_size = is_main ? 256u * 1024 * 1024 : 64u * 1024 * 1024;
#endif
    t->jit = std::make_unique<A32::Jit>(cfg);
    t->jit->Regs()[13] = t->stack_base + t->stack_size;
    t->jit->SetCpsr(0x10);
    t_current = t;
    {
        std::lock_guard lk(g_threads_mutex);
        g_threads.push_back(t);
    }
    return t;
}

void detach_current_thread() {
    GuestThread* t = t_current;
    if (!t) return;
    t_current = nullptr;
    {
        std::lock_guard lk(g_threads_mutex);
        g_threads.erase(std::remove(g_threads.begin(), g_threads.end(), t), g_threads.end());
    }
    free_processor_id(t->processor_id);
    if (t->owns_stack) mem::heap_free(t->stack_base);
    mem::heap_free(t->tls_base);
    delete t;
}

GuestThread& current() {
    if (!t_current) fatal("guest call from a thread without guest context");
    return *t_current;
}
GuestThread* current_or_null() { return t_current; }

void note_call(GuestThread& t, const char* name) {
    if (t_tracing && t_trace) (*t_trace)[name]++;
    if (g_trace_all.load(std::memory_order_relaxed) && t.id != 1000) {
        std::lock_guard lk(g_trace_all_mutex);
        g_trace_all_counts[std::to_string(t.id) + ":" + name]++;
    }
}
void trace_all(bool on) {
    if (on) {
        std::lock_guard lk(g_trace_all_mutex);
        g_trace_all_counts.clear();
        g_trace_all = true;
        return;
    }
    g_trace_all = false;
    std::lock_guard lk(g_trace_all_mutex);
    std::vector<std::pair<int, std::string>> v;
    for (auto& [k, n] : g_trace_all_counts) v.emplace_back(n, k);
    std::sort(v.rbegin(), v.rend());
    std::string line;
    for (auto& [n, k] : v) line += k + " x" + std::to_string(n) + ", ";
    LOGI("other threads: %s", line.c_str());
}
u64 g_prof_filtered0 = 0;
u64 g_prof_insns0 = 0, g_prof_reads0 = 0, g_prof_excl0 = 0;
void profile_begin() {
    g_prof_filtered0 = hle::gl_filtered_count();
    if (t_current) {
        g_prof_insns0 = t_current->guest_insns;
        g_prof_reads0 = t_current->slow_reads;
        g_prof_excl0 = t_current->excl_writes;
    }
    std::memset(g_prof_ns, 0, sizeof g_prof_ns);
    std::memset(g_prof_n, 0, sizeof g_prof_n);
    g_prof = true;
}
void profile_end(int frames, double tick_ms_total) {
    g_prof = false;
    if (frames <= 0) return;
    std::vector<std::pair<u64, u32>> v;
    u64 total_ns = 0, total_n = 0;
    for (u32 i = 0; i < kProfSlots; i++)
        if (g_prof_n[i]) {
            v.emplace_back(g_prof_ns[i], i);
            total_ns += g_prof_ns[i];
            total_n += g_prof_n[i];
        }
    std::sort(v.rbegin(), v.rend());
    double host_ms = total_ns / 1e6 / frames, tick_ms = tick_ms_total / frames;
    LOGI("profile (%d frames, main thread): tick %.2f ms/frame = host calls %.2f ms (%.0f calls) + guest code %.2f ms",
         frames, tick_ms, host_ms, (double)total_n / frames, tick_ms - host_ms);
    std::string line;
    for (size_t k = 0; k < v.size() && k < 200; k++) {
        const thunks::Entry* e = thunks::entry(v[k].second);
        char b[160];
        std::snprintf(b, sizeof b, "%s %.3f ms x%.0f; ", e ? e->name.c_str() : "?", v[k].first / 1e6 / frames,
                      (double)g_prof_n[v[k].second] / frames);
        line += b;
        if ((k + 1) % 8 == 0) {
            LOGI("profile: %s", line.c_str());
            line.clear();
        }
    }
    if (!line.empty()) LOGI("profile: %s", line.c_str());
    u64 cd, ca, ci;
    hle::gl_client_stats(cd, ca, ci);
    if (t_current && t_current->guest_insns)
        LOGI("profile: guest instructions: %.2f million per frame (main thread)",
             (double)(t_current->guest_insns - g_prof_insns0) / frames / 1e6);
    if (t_current)
        LOGI("profile: memory reads through callbacks %.0f, exclusive stores %.0f per frame (main thread)",
             (double)(t_current->slow_reads - g_prof_reads0) / frames,
             (double)(t_current->excl_writes - g_prof_excl0) / frames);
    LOGI("profile: redundant GL calls dropped: %.0f per frame",
         (double)(hle::gl_filtered_count() - g_prof_filtered0) / frames);
    LOGI("profile: client-memory vertex arrays: %llu draws, %llu pointers; client index arrays: %llu draws (since start)",
         (unsigned long long)cd, (unsigned long long)ca, (unsigned long long)ci);
}

void trace_calls_begin() {
    if (!t_trace) t_trace = new std::map<std::string, int>;
    t_trace->clear();
    t_tracing = true;
}
void trace_calls_end(const char* label) {
    t_tracing = false;
    if (!t_trace) return;
    std::vector<std::pair<int, std::string>> v;
    for (auto& [k, n] : *t_trace) v.emplace_back(n, k);
    std::sort(v.rbegin(), v.rend());
    std::string line;
    for (auto& [n, k] : v) line += k + " x" + std::to_string(n) + ", ";
    LOGI("calls during %s: %s", label, line.c_str());
}

void dump_threads() {
    std::lock_guard lk(g_threads_mutex);
    LOGI("---- %zu guest threads ----", g_threads.size());
    for (GuestThread* t : g_threads) {
        const char* th = t->in_thunk;
        if (th) {
            LOGI("thread %u: in %s, called from %s (%llu host calls)", t->id, th,
                 elf::describe(t->in_thunk_lr).c_str(), (unsigned long long)t->thunk_calls);
        } else {
            LOGI("thread %u: running guest code (%llu host calls)", t->id, (unsigned long long)t->thunk_calls);
        }
    }
}

void crash_report(GuestThread& t, const char* reason) {
    auto& r = t.jit->Regs();
    LOGE("==== guest crash on thread %u: %s ====", t.id, reason);
    for (int i = 0; i < 16; i += 4)
        LOGE("  r%-2d=%08X r%-2d=%08X r%-2d=%08X r%-2d=%08X", i, r[i], i + 1, r[i + 1], i + 2, r[i + 2], i + 3,
             r[i + 3]);
    LOGE("  cpsr=%08X  pc: %s", t.jit->Cpsr(), elf::describe(r[15]).c_str());
    LOGE("  lr: %s", elf::describe(r[14]).c_str());
    // Heuristic backtrace: words on the stack that point just after a call
    // instruction inside a loaded module.
    u32 sp = r[13];
    int found = 0;
    for (u32 off = 0; off < 4096 && found < 16; off += 4) {
        u32 a = sp + off;
        if (a < mem::NULL_GUARD_END) break;
        u32 v = mem::r32(a);
        elf::Module* m = elf::module_for(v);
        if (m && (v & ~1u) > m->base + 4) {
            LOGE("  stack[%04X]: %s", off, elf::describe(v).c_str());
            found++;
        }
    }
}

}  // namespace cpu
