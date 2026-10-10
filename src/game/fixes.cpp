// Fixes for bugs in the game's own code (NFS:MW 1.3.128), applied after
// loading. Each checks the original code first, so a different build of the
// game is left untouched.
#include "game/fixes.h"

#include <atomic>
#include <chrono>
#include <vector>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "core/cpu.h"
#include "core/elf.h"
#include "core/hook.h"
#include "core/memory.h"
#include "core/thunks.h"
#include "game/rtti.h"

namespace game::fixes {

// ---- texture uploads from loader threads -----------------------------------
// The game records GPU commands into buffers that the main thread runs once
// per frame. A loader thread reserves room for a
// texture-upload command, and before it has written the command the main
// thread swaps the buffers and runs it (measured: 0.16 ms apart). The game
// does not lock against this. Loader threads therefore no longer record
// uploads themselves: they hand the texture to the main thread, which records
// the upload at the start of its next frame, where nothing can interleave.
namespace upload {
std::mutex g_mu;
std::vector<u32> g_queue;           // textures waiting (one reference held each)
u32 g_main_thread = 0;
u32 g_original = 0, g_release = 0;  // guest functions
std::atomic<s64> g_last_service{0};
u64 g_deferred = 0;

s64 now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void emit_mov32(std::vector<u32>& c, u32 reg, u32 v) {
    c.push_back(0xE3000000 | ((v >> 12) & 0xF) << 16 | reg << 12 | (v & 0xFFF));
    c.push_back(0xE3400000 | ((v >> 28) & 0xF) << 16 | reg << 12 | ((v >> 16) & 0xFFF));
}
u32 emit(const std::vector<u32>& c) {
    u32 at = mem::static_alloc((u32)c.size() * 4, 8);
    for (size_t i = 0; i < c.size(); i++) mem::w32(at + (u32)i * 4, c[i]);
    return at;
}

bool install(elf::Module* m) {
    constexpr u32 kUpload = 0x473690;  // isis::Texture vtable slot 8
    u32 upload = m->base + kUpload;
    if (mem::r32(upload) != 0xE92D4FF0 || mem::r32(upload + 4) != 0xE28DB01C) return false;
    g_original = upload;
    g_main_thread = cpu::current().id;

    u32 query = thunks::add("tex_upload_query", [](cpu::GuestThread& t) {
        // Defer when called off the main thread, unless the main thread has
        // not come round for a while (then it may be waiting for this thread).
        bool defer = t.id != g_main_thread && now_ms() - g_last_service.load() < 250;
        t.regs()[0] = defer ? 1 : 0;
    });
    u32 enqueue = thunks::add("tex_upload_enqueue", [](cpu::GuestThread& t) {
        std::lock_guard lk(g_mu);
        g_queue.push_back(t.regs()[0]);
        g_deferred++;
    });
    constexpr u32 IP = 12;
    std::vector<u32> c;
    c.push_back(0xE92D4013);  // push {r0, r1, r4, lr}
    emit_mov32(c, IP, query);
    c.push_back(0xE12FFF3C);  // blx ip          -> r0 = defer?
    c.push_back(0xE3500000);  // cmp r0, #0
    c.push_back(0x0A000000);  // beq call_original (offset filled in below)
    c.push_back(0xE59D0000);  // ldr r0, [sp]     this
    c.push_back(0xE2801004);  // add r1, r0, #4   &refcount
    c.push_back(0xF57FF05B);  // dmb ish
    c.push_back(0xE1912F9F);  // ldrex r2, [r1]
    c.push_back(0xE2822001);  // add r2, r2, #1
    c.push_back(0xE1813F92);  // strex r3, r2, [r1]
    c.push_back(0xE3530000);  // cmp r3, #0
    c.push_back(0x1AFFFFFA);  // bne ldrex
    c.push_back(0xF57FF05B);  // dmb ish
    emit_mov32(c, IP, enqueue);
    c.push_back(0xE12FFF3C);  // blx ip          (r0 = this)
    c.push_back(0xE28DD008);  // add sp, sp, #8
    c.push_back(0xE8BD8010);  // pop {r4, pc}
    // call_original:
    size_t call_original = c.size();
    c.push_back(0xE8BD4013);  // pop {r0, r1, r4, lr}
    emit_mov32(c, IP, g_original);
    c.push_back(0xE12FFF1C);  // bx ip
    // beq (index 5) -> call_original
    s32 off = (s32)call_original - (5 + 2);
    c[5] = 0x0A000000 | ((u32)off & 0xFFFFFF);
    u32 tramp = emit(c);

    // release(texture): drop the reference taken above; delete on the last one.
    std::vector<u32> r = {
        0xE2801004,  // add r1, r0, #4
        0xF57FF05B,  // dmb ish
        0xE1912F9F,  // ldrex r2, [r1]
        0xE2423001,  // sub r3, r2, #1
        0xE181CF93,  // strex ip, r3, [r1]
        0xE35C0000,  // cmp ip, #0
        0x1AFFFFFA,  // bne ldrex
        0xF57FF05B,  // dmb ish
        0xE3520001,  // cmp r2, #1
        0x112FFF1E,  // bxne lr
        0xE5901000,  // ldr r1, [r0]       vtable
        0xE5911004,  // ldr r1, [r1, #4]   deleting destructor
        0xE12FFF11,  // bx r1
    };
    g_release = emit(r);

    int n = 0;
    for (const rtti::VTable& v : rtti::derived_vtables(m, "N2im4isis7TextureE")) {
        u32 slot = v.vtable + 8 * 4;
        if (mem::r32(slot) == g_original) {
            mem::w32(slot, tramp);
            n++;
        }
    }
    g_last_service = now_ms();
    LOGI("game fix applied: texture uploads from loader threads go through the main thread (%d classes)", n);
    return n > 0;
}

void service() {
    if (!g_original) return;
    g_last_service = now_ms();
    std::vector<u32> q;
    {
        std::lock_guard lk(g_mu);
        q.swap(g_queue);
    }
    static bool logged = false;
    if (!q.empty() && !logged) {
        logged = true;
        LOGI("texture uploads from loader threads now recorded by the main thread");
    }
    auto& t = cpu::current();
    for (u32 tex : q) {
        t.call(g_original, {tex});
        t.call(g_release, {tex});
    }
}
}  // namespace upload

namespace {
// NFSMW_DIAG_CMDBUF=1: log who records texture-upload commands and catch one
// that runs before it was filled in (debugging aid).
void diag_cmdbuf(elf::Module* m) {
    struct Rec {
        u32 thread;
        double t;
    };
    static std::mutex mu;
    static std::unordered_map<u32, Rec> recs;
    static u32 tex_done_fn = m->base + 0x473a3c;
    static auto t0 = std::chrono::steady_clock::now();
    auto now = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    u32 on_alloc = thunks::add("diag_cmd_alloc", [now](cpu::GuestThread& t) {
        auto& R = t.regs();
        if (R[2] != tex_done_fn) return;
        std::lock_guard lk(mu);
        recs[R[0]] = {t.id, now()};
    });
    u32 on_exec = thunks::add("diag_cmd_tex_done", [now](cpu::GuestThread& t) {
        u32 payload = t.regs()[0];
        if (mem::r32(payload) != 0) return;
        std::lock_guard lk(mu);
        auto it = recs.find(payload);
        LOGE("diag: unfilled texture command at %08X (thread %u, now %.3f ms); allocated by thread %u at %.3f ms",
             payload, t.id, now(), it != recs.end() ? it->second.thread : 0, it != recs.end() ? it->second.t : -1.0);
    });
    bool ok = hook::inline_post(m->base + 0x4679cc, on_alloc) && hook::inline_pre(m->base + 0x473a3c, on_exec);
    LOGI("diag: command buffer hooks %s", ok ? "installed" : "FAILED");
}
}  // namespace

// ---- null object in a timing function -------------------------------------
// The function at 0x564A9C measures a time span with the game's clock, then
// stores the result through an object it reaches from a global:
// [[[G + 4] + 0x24] + 0x3C] (read) and [... + 0x40] (write). In one desktop
// test run, about 20 s after start-up (loading into the first race), the
// object at +0x24 was not there yet and the game crashed reading address 0x3C. The function now skips those
// two accesses (and the store of the result) when that object is missing; the
// rest, including resetting its counters, runs as before.
namespace timing_null {
bool install(elf::Module* m) {
    const u32 b = m->base;
    if (mem::r32(b + 0x564AF4) != 0xE5900024 || mem::r32(b + 0x564AF8) != 0xE590203C ||
        mem::r32(b + 0x564B24) != 0xE5801040 || mem::r32(b + 0x564B28) != 0xE3A00000 ||
        mem::r32(b + 0x564AFC) != 0xE59F3044 || mem::r32(b + 0x564B04) != 0xE59F5040 ||
        mem::r32(b + 0x564B08) != 0xE59F4040 || mem::r32(b + 0x564B0C) != 0xE08F3003 ||
        mem::r32(b + 0x564B10) != 0xE08F5005 || mem::r32(b + 0x564B14) != 0xE08F4004)
        return false;
    // The three globals the end of the function clears (pc-relative there).
    const u32 g3 = b + 0x564B0C + 8 + mem::r32(b + 0x564B48);
    const u32 g5 = b + 0x564B10 + 8 + mem::r32(b + 0x564B4C);
    const u32 g4 = b + 0x564B14 + 8 + mem::r32(b + 0x564B50);
    std::vector<u32> c;
    c.push_back(0xE5900024);  // ldr r0, [r0, #0x24]
    c.push_back(0xE3500000);  // cmp r0, #0
    c.push_back(0x0A000002);  // beq skip (+2 words after the next two)
    c.push_back(0xE590203C);  // ldr r2, [r0, #0x3c]
    c.push_back(0xE51FF004);  // ldr pc, [pc, #-4]
    c.push_back(b + 0x564AFC);  //   back to the original code
    // skip: set r3/r4/r5 as the original code would, then clear the globals
    upload::emit_mov32(c, 3, g3);
    upload::emit_mov32(c, 4, g4);
    upload::emit_mov32(c, 5, g5);
    c.push_back(0xE51FF004);  // ldr pc, [pc, #-4]
    c.push_back(b + 0x564B28);  //   mov r0,#0; str r0,[r4]; strb r0,[r3]; str r0,[r5]; return
    const u32 stub = upload::emit(c);
    mem::w32(b + 0x564AF4, 0xE51FF004);  // ldr pc, [pc, #-4]
    mem::w32(b + 0x564AF8, stub);
    return true;
}
}  // namespace timing_null

void apply() {
    elf::Module* m = elf::find("libapp.so");
    if (!m) return;
    if (!timing_null::install(m)) LOGW("game fix skipped (different game build): timing function null check");
    else LOGI("game fix applied: null check in a timing function");
    if (!std::getenv("NFSMW_NO_UPLOAD_FIX") && !upload::install(m))
        LOGW("game fix skipped (different game build): texture upload hand-off");
    if (std::getenv("NFSMW_DIAG_CMDBUF")) diag_cmdbuf(m);
}

void frame() { upload::service(); }

}  // namespace game::fixes
