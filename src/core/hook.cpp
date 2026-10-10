#include "core/hook.h"

#include <vector>

#include "core/memory.h"

namespace hook {

namespace {
constexpr u32 IP = 12;

void mov32(std::vector<u32>& c, u32 reg, u32 v) {
    c.push_back(0xE3000000 | ((v >> 12) & 0xF) << 16 | reg << 12 | (v & 0xFFF));          // movw
    c.push_back(0xE3400000 | ((v >> 28) & 0xF) << 16 | reg << 12 | ((v >> 16) & 0xFFF));  // movt
}

u32 emit(const std::vector<u32>& c) {
    u32 at = mem::static_alloc((u32)c.size() * 4, 8);
    for (size_t i = 0; i < c.size(); i++) mem::w32(at + (u32)i * 4, c[i]);
    return at;
}

bool branch(u32 from, u32 to, u32& insn) {
    s64 off = (s64)to - (s64)(from + 8);
    if (off < -(32 << 20) || off >= (32 << 20) || (off & 3)) return false;
    insn = 0xEA000000 | ((u32)(off >> 2) & 0xFFFFFF);
    return true;
}
}  // namespace

u32 post(u32 original, u32 host_stub) {
    std::vector<u32> c;
    c.push_back(0xE92D4013);  // push {r0, r1, r4, lr}
    mov32(c, IP, original);
    c.push_back(0xE12FFF3C);  // blx ip
    c.push_back(0xE59D1000);  // ldr r1, [sp]
    c.push_back(0xE59D2004);  // ldr r2, [sp, #4]
    mov32(c, IP, host_stub);
    c.push_back(0xE12FFF3C);  // blx ip
    c.push_back(0xE28DD008);  // add sp, sp, #8
    c.push_back(0xE8BD8010);  // pop {r4, pc}
    return emit(c);
}

u32 pre(u32 original, u32 host_stub) {
    std::vector<u32> c;
    c.push_back(0xE92D400F);  // push {r0-r3, lr}  (+ pad below)
    c.push_back(0xE24DD004);  // sub sp, sp, #4    (keep 8-byte alignment)
    mov32(c, IP, host_stub);
    c.push_back(0xE12FFF3C);  // blx ip
    c.push_back(0xE28DD004);  // add sp, sp, #4
    c.push_back(0xE8BD400F);  // pop {r0-r3, lr}
    mov32(c, IP, original);
    c.push_back(0xE12FFF1C);  // bx ip
    return emit(c);
}

namespace {
// Copy of the function's first instruction followed by a branch back.
u32 entry_copy(u32 func) {
    u32 first = mem::r32(func);
    if ((first & 0xFFFF0000) != 0xE92D0000) return 0;  // push {...} only
    u32 orig = mem::static_alloc(8, 8);
    u32 back;
    if (!branch(orig + 4, func + 4, back)) return 0;
    mem::w32(orig, first);
    mem::w32(orig + 4, back);
    return orig;
}

bool install(u32 func, u32 orig, u32 tramp) {
    u32 jump;
    if (!orig || !branch(func, tramp, jump)) return false;
    mem::w32(func, jump);
    return true;
}
}  // namespace

bool inline_post(u32 func, u32 host_stub) {
    u32 orig = entry_copy(func);
    return orig && install(func, orig, post(orig, host_stub));
}

bool inline_pre(u32 func, u32 host_stub) {
    u32 orig = entry_copy(func);
    return orig && install(func, orig, pre(orig, host_stub));
}

}  // namespace hook
