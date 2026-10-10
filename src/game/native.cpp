// Native versions of hot spots in the game's code (NFS:MW 1.3.128, libapp.so).
//
// In a race the game streams its world from the OBB on the main thread. The
// OBB is a zip archive with encrypted ("traditional PKWARE") entries, read
// through the game's copy of minizip: every byte is decrypted, copied and
// CRC-checked by translated ARM code, one byte at a time. Sampling the main
// thread in a race put about a tenth of its time there. Run natively, the same
// work takes a small fraction of that:
//
//  - unzReadCurrentFile's decryption loop (libapp.so+0x647310..0x647390)
//  - its byte-by-byte copy loop for stored entries (+0x6473F8..0x647414)
//  - zlib's crc32() (+0x65F6C8)
//
// Each patch site is checked word for word first; a different game build is
// left alone. The results are bit-identical to the game's own code.
#include "game/native.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

#include "core/cpu.h"
#include "core/elf.h"
#include "core/memory.h"
#include "core/thunks.h"

namespace game::native {

namespace {

bool matches(u32 at, const std::vector<u32>& words) {
    for (size_t i = 0; i < words.size(); i++)
        if (mem::r32(at + (u32)(i * 4)) != words[i]) return false;
    return true;
}
u32 emit(const std::vector<u32>& c) {
    u32 at = mem::static_alloc((u32)c.size() * 4, 8);
    for (size_t i = 0; i < c.size(); i++) mem::w32(at + (u32)i * 4, c[i]);
    return at;
}
// "ldr pc, [pc, #-4] ; .word target" at `at` (8 bytes).
void jump(u32 at, u32 target) {
    mem::w32(at, 0xE51FF004);
    mem::w32(at + 4, target);
}
bool in_guest(u32 addr, u32 len) { return addr >= mem::NULL_GUARD_END && addr <= mem::g_size && len <= mem::g_size - addr; }

// ---- crc32 ---------------------------------------------------------------------
u32 g_crc_table[8][256];
void init_crc_tables() {
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[0][i] = c;
    }
    for (u32 i = 0; i < 256; i++)
        for (int t = 1; t < 8; t++) g_crc_table[t][i] = (g_crc_table[t - 1][i] >> 8) ^ g_crc_table[0][g_crc_table[t - 1][i] & 0xFF];
}
// zlib crc32(crc, buf, len): the standard reflected CRC-32 (0xEDB88320).
u32 crc32_update(u32 crc, const u8* p, size_t n) {
    crc = ~crc;
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
    // The Switch's CPU has CRC-32 instructions for exactly this polynomial.
    while (n && ((uintptr_t)p & 7)) crc = __crc32b(crc, *p++), n--;
    for (; n >= 8; n -= 8, p += 8) {
        u64 v;
        std::memcpy(&v, p, 8);
        crc = __crc32d(crc, v);
    }
    while (n--) crc = __crc32b(crc, *p++);
#else
    for (; n >= 8; n -= 8, p += 8) {
        u32 a, b;
        std::memcpy(&a, p, 4);
        std::memcpy(&b, p + 4, 4);
        a ^= crc;
        crc = g_crc_table[7][a & 0xFF] ^ g_crc_table[6][(a >> 8) & 0xFF] ^ g_crc_table[5][(a >> 16) & 0xFF] ^
              g_crc_table[4][a >> 24] ^ g_crc_table[3][b & 0xFF] ^ g_crc_table[2][(b >> 8) & 0xFF] ^
              g_crc_table[1][(b >> 16) & 0xFF] ^ g_crc_table[0][b >> 24];
    }
    while (n--) crc = g_crc_table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
#endif
    return ~crc;
}

bool install_crc32(elf::Module* m) {
    const u32 f = m->base + 0x65F6C8;
    // cmp r1,#0 ; moveq r0,#0 ; bxeq lr ; push {r4-r11,lr} ; ... ; mvn r3, r0
    if (!matches(f, {0xE3510000, 0x03A00000, 0x012FFF1E, 0xE92D4FF0, 0xE28DB01C, 0xE24DD008, 0xE1E03000}))
        return false;
    u32 thunk = thunks::add("native_crc32", [](cpu::GuestThread& t) {
        u32 crc = t.regs()[0], buf = t.regs()[1], len = t.regs()[2];
        if (!buf) {
            t.regs()[0] = 0;
            return;
        }
        if (!in_guest(buf, len)) fatal("crc32: bad buffer %08X+%u", buf, len);
        t.regs()[0] = crc32_update(crc, mem::host(buf), len);
    });
    jump(f, thunk);
    return true;
}

// ---- unzReadCurrentFile: decryption ----------------------------------------------
// r8 = unz file state (keys at +0xA4/+0xA8/+0xAC, CRC table pointer at +0xB0),
// [r7] = buffer, r10 = byte count (> 0). After the loop the code at +0x647394
// reloads everything it uses (r3-r6, r12 and lr are dead there).
bool install_decrypt(elf::Module* m) {
    const u32 loop = m->base + 0x647310, after = m->base + 0x647394;
    static const std::vector<u32> code = {
        0xE3A00000, 0xE59810AC, 0xE5973000, 0xE001100C, 0xE59820B0, 0xE2215003, 0xE3811002, 0xE0010591,
        0xE7D35000, 0xE0251421, 0xE7C31000, 0xE59830A4, 0xE59850A8, 0xE0236001, 0xE59840AC, 0xE6EF6076,
        0xE7926106, 0xE0263423, 0xE58830A4, 0xE6EF6074, 0xE6E53073, 0xE0030E93, 0xE2833001, 0xE58830A8,
        0xE0263C23, 0xE7922103, 0xE0222424, 0xE58820AC, 0xE5972000, 0xE7C21000, 0xE2800001, 0xE150000A,
        0x3AFFFFDF};
    // Before the loop: the multiplier (0x08088405) in lr and the 0xFFFD mask in r12.
    if (!matches(loop, code) || !matches(loop - 0x14, {0xE308E405, 0xE30FCFFD, 0xE340E808, 0xE3500000, 0x0A000020}))
        return false;
    u32 thunk = thunks::add("native_zip_decrypt", [](cpu::GuestThread& t) {
        const u32 st = t.regs()[0], buf = t.regs()[1], n = t.regs()[2];
        if (!in_guest(st + 0xA4, 16) || !in_guest(buf, n)) fatal("zip decrypt: bad state %08X / buffer %08X+%u", st, buf, n);
        const u32 tab_addr = mem::r32(st + 0xB0);
        if (!in_guest(tab_addr, 1024)) fatal("zip decrypt: bad CRC table %08X", tab_addr);
        u32 tab[256];
        std::memcpy(tab, mem::host(tab_addr), sizeof tab);
        u32 k0 = mem::r32(st + 0xA4), k1 = mem::r32(st + 0xA8), k2 = mem::r32(st + 0xAC);
        u8* p = mem::host(buf);
        for (u32 i = 0; i < n; i++) {
            const u32 temp = (k2 & 0xFFFD) | 2;
            const u8 c = (u8)(p[i] ^ ((temp * (temp ^ 1)) >> 8));
            p[i] = c;
            k0 = tab[(k0 ^ c) & 0xFF] ^ (k0 >> 8);
            k1 = (k1 + (k0 & 0xFF)) * 0x08088405u + 1;
            k2 = tab[(k2 ^ (k1 >> 24)) & 0xFF] ^ (k2 >> 8);
        }
        mem::w32(st + 0xA4, k0);
        mem::w32(st + 0xA8, k1);
        mem::w32(st + 0xAC, k2);
        t.regs()[0] = n;
    });
    const u32 tramp = emit({
        0xE1A00008,  // mov r0, r8        state
        0xE5971000,  // ldr r1, [r7]      buffer
        0xE1A0200A,  // mov r2, r10       count
        0xE59FC008,  // ldr ip, [pc, #8]
        0xE12FFF3C,  // blx ip
        0xE51FF004,  // ldr pc, [pc, #-4]
        after,
        thunk,
    });
    jump(loop, tramp);
    return true;
}

// ---- unzReadCurrentFile: copy of stored data ---------------------------------------
// for (i = 0; i < r10; i++) ((u8*)[r7+16])[i] = ((u8*)[r7+4])[i];  r10 > 0.
// The code after it (+0x647418) reloads r0-r2.
bool install_copy(elf::Module* m) {
    const u32 loop = m->base + 0x6473F8, after = m->base + 0x647418;
    if (!matches(loop, {0xE3A00000, 0xE5971004, 0xE5972010, 0xE7D11000, 0xE7C21000, 0xE2800001, 0xE150000A,
                        0x3AFFFFF8}) ||
        !matches(loop - 8, {0xE35A0000, 0x0A000007}))  // cmp r10, #0 ; beq after
        return false;
    u32 thunk = thunks::add("native_zip_copy", [](cpu::GuestThread& t) {
        const u32 dst = t.regs()[0], src = t.regs()[1], n = t.regs()[2];
        if (!in_guest(dst, n) || !in_guest(src, n)) fatal("zip copy: bad buffers %08X <- %08X +%u", dst, src, n);
        u8* d = mem::host(dst);
        const u8* s = mem::host(src);
        if (d + n <= s || s + n <= d || d < s) {
            std::memmove(d, s, n);  // same result as the forward byte loop
        } else {
            for (u32 i = 0; i < n; i++) d[i] = s[i];  // overlapping, destination after the source
        }
        t.regs()[0] = n;
    });
    const u32 tramp = emit({
        0xE5970010,  // ldr r0, [r7, #16]  destination
        0xE5971004,  // ldr r1, [r7, #4]   source
        0xE1A0200A,  // mov r2, r10        count
        0xE59FC008,  // ldr ip, [pc, #8]
        0xE12FFF3C,  // blx ip
        0xE51FF004,  // ldr pc, [pc, #-4]
        after,
        thunk,
    });
    jump(loop, tramp);
    return true;
}

}  // namespace

void apply() {
    if (std::getenv("NFSMW_NO_NATIVE")) {
        LOGW("native code: off (NFSMW_NO_NATIVE)");
        return;
    }
    elf::Module* m = elf::find("libapp.so");
    if (!m) return;
    init_crc_tables();
    const bool crc = install_crc32(m), dec = install_decrypt(m), copy = install_copy(m);
    LOGI("native code for OBB reading: crc32 %s, decryption %s, stored-data copy %s", crc ? "on" : "skipped",
         dec ? "on" : "skipped", copy ? "on" : "skipped");
}

}  // namespace game::native
