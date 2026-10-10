#include "core/thunks.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace thunks {

namespace {
constexpr u32 MAX_THUNKS = (mem::THUNK_END - mem::THUNK_BASE) / 8;
Entry* g_table[MAX_THUNKS];
std::atomic<u32> g_count{0};
std::mutex g_mutex;
std::unordered_map<std::string, u32> g_by_name;  // name -> guest address
}  // namespace

u32 add(const std::string& name, Fn fn, bool reentrant) {
    std::lock_guard lk(g_mutex);
    auto it = g_by_name.find(name);
    if (it != g_by_name.end()) {
        // Re-registration replaces the implementation (lets later modules override).
        u32 idx = (it->second - mem::THUNK_BASE) / 8;
        if (it->second >= mem::THUNK_BASE && it->second < mem::THUNK_END && g_table[idx]) {
            g_table[idx]->fn = std::move(fn);
            g_table[idx]->reentrant = reentrant;
            g_table[idx]->missing = false;
            return it->second;
        }
    }
    u32 idx = g_count.load();
    if (idx >= MAX_THUNKS) fatal("too many thunks");
    auto* e = new Entry{name, std::move(fn), reentrant, mem::THUNK_BASE + idx * 8, false};
    mem::w32(e->addr, 0xEF000000u | idx);  // svc #idx
    mem::w32(e->addr + 4, 0xE12FFF1Eu);    // bx lr
    g_table[idx] = e;
    g_count.store(idx + 1);
    g_by_name[name] = e->addr;
    return e->addr;
}

void add_data(const std::string& name, u32 guest_addr) {
    std::lock_guard lk(g_mutex);
    g_by_name[name] = guest_addr;
}

u32 lookup(const std::string& name) {
    std::lock_guard lk(g_mutex);
    auto it = g_by_name.find(name);
    if (it == g_by_name.end()) return 0;
    if (it->second >= mem::THUNK_BASE && it->second < mem::THUNK_END) {
        Entry* e = g_table[(it->second - mem::THUNK_BASE) / 8];
        if (e && e->missing) return 0;
    }
    return it->second;
}

u32 make_missing(const std::string& name) {
    {
        std::lock_guard lk(g_mutex);
        auto it = g_by_name.find(name);
        if (it != g_by_name.end()) return it->second;
    }
    auto warned = std::make_shared<std::atomic<bool>>(false);
    u32 a = add(name, [name, warned](cpu::GuestThread& t) {
        if (!warned->exchange(true)) {
            LOGW("call to unimplemented function %s (lr=0x%08X)", name.c_str(), t.regs()[14]);
        }
        t.regs()[0] = 0;
        t.regs()[1] = 0;
    });
    std::lock_guard lk(g_mutex);
    g_table[(a - mem::THUNK_BASE) / 8]->missing = true;
    return a;
}

const Entry* entry(u32 index) { return index < g_count.load() ? g_table[index] : nullptr; }

void dispatch(cpu::GuestThread& t, u32 index) {
    const Entry* e = entry(index);
    if (!e) {
        cpu::crash_report(t, "SVC with unknown thunk index");
        fatal("bad thunk index %u", index);
    }
    const char* prev = t.in_thunk;
    u32 prev_lr = t.in_thunk_lr;
    t.in_thunk = e->name.c_str();
    t.in_thunk_lr = t.regs()[14];
    cpu::note_call(t, e->name.c_str());
    e->fn(t);
    t.in_thunk = prev;
    t.in_thunk_lr = prev_lr;
}

const char* name_at(u32 addr) {
    if (addr < mem::THUNK_BASE || addr >= mem::THUNK_END) return nullptr;
    const Entry* e = entry((addr - mem::THUNK_BASE) / 8);
    return e ? e->name.c_str() : nullptr;
}

}  // namespace thunks
