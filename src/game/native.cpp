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
// and, for loading screens (0.2.7), where a loading thread spent about a sixth
// of its time decompressing and a twelfth hashing:
//  - zlib's inflate_fast() (+0x6674FC)
//  - the game's FNV-1 hash of loaded data (+0x4EDAD4)
//
// Each patch site is checked word for word first; a different game build is
// left alone. The results are bit-identical to the game's own code
// (NFSMW_VERIFY_NATIVE=1 runs both and compares them).
#include "game/native.h"

#include <algorithm>
#include <atomic>
#include <mutex>
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

// ---- zlib inflate_fast ------------------------------------------------------------
// The game's zlib (1.2.5.1 or later, statically linked in libapp.so) decodes
// compressed OBB entries; inflate_fast() decodes most of the bytes and was the
// hottest function while loading (about a sixth of a loading thread's time).
// This is the same algorithm, adapted from zlib 1.2.8's inffast.c
// (Copyright (C) 1995-2013 Mark Adler; zlib license, see res/licenses/zlib.txt):
// altered to work on the game's own z_stream and inflate_state in guest memory.
//
// Guest layouts (32-bit): z_stream: next_in 0x00, avail_in 0x04, next_out 0x0C,
// avail_out 0x10, msg 0x18, state 0x1C. inflate_state: mode 0x00, wsize 0x28,
// whave 0x2C, wnext 0x30, window 0x34, hold 0x38, bits 0x3C, lencode 0x4C,
// distcode 0x50, lenbits 0x54, distbits 0x58, sane 0x1BC0.
namespace zfast {
constexpr u32 kModeType = 11, kModeBad = 29;  // inflate_mode values of this zlib
u32 g_msg_too_far = 0, g_msg_dist = 0, g_msg_lit = 0;
struct Code {
    u8 op, bits;
    u16 val;
};
static_assert(sizeof(Code) == 4);

void inflate_fast(u32 strm, u32 start) {
    if (!in_guest(strm, 0x38)) fatal("inflate_fast: bad stream %08X", strm);
    const u32 st = mem::r32(strm + 0x1C);
    if (!in_guest(st, 0x1BC4)) fatal("inflate_fast: bad state %08X", st);
    const u32 next_in = mem::r32(strm), avail_in = mem::r32(strm + 4);
    const u32 next_out = mem::r32(strm + 0xC), avail_out = mem::r32(strm + 0x10);
    if (!in_guest(next_in, avail_in) || !in_guest(next_out, avail_out) || start < avail_out ||
        !in_guest(next_out - (start - avail_out), start))
        fatal("inflate_fast: bad buffers in %08X+%u out %08X+%u", next_in, avail_in, next_out, avail_out);
    const u32 wsize = mem::r32(st + 0x28), whave = mem::r32(st + 0x2C), wnext = mem::r32(st + 0x30);
    const u32 window_addr = mem::r32(st + 0x34);
    if (wsize && !in_guest(window_addr, wsize)) fatal("inflate_fast: bad window %08X+%u", window_addr, wsize);
    const u32 sane = mem::r32(st + 0x1BC0);
    const Code* lcode = reinterpret_cast<const Code*>(mem::host(mem::r32(st + 0x4C)));
    const Code* dcode = reinterpret_cast<const Code*>(mem::host(mem::r32(st + 0x50)));
    const u32 lmask = (1u << mem::r32(st + 0x54)) - 1, dmask = (1u << mem::r32(st + 0x58)) - 1;

    const u8* in = mem::host(next_in);
    const u8* const in0 = in;
    const u8* const last = in + (avail_in - 5);
    u8* out = mem::host(next_out);
    u8* const out0 = out;
    u8* const beg = out - (start - avail_out);
    u8* const end = out + (avail_out - 257);
    const u8* const window = wsize ? mem::host(window_addr) : nullptr;
    u32 hold = mem::r32(st + 0x38), bits = mem::r32(st + 0x3C);
    u32 op, len, dist;
    const u8* from;
    Code here;

    do {
        if (bits < 15) {
            hold += (u32)(*in++) << bits;
            bits += 8;
            hold += (u32)(*in++) << bits;
            bits += 8;
        }
        here = lcode[hold & lmask];
    dolen:
        op = here.bits;
        hold >>= op;
        bits -= op;
        op = here.op;
        if (op == 0) {  // literal
            *out++ = (u8)here.val;
        } else if (op & 16) {  // length base
            len = here.val;
            op &= 15;
            if (op) {
                if (bits < op) {
                    hold += (u32)(*in++) << bits;
                    bits += 8;
                }
                len += hold & ((1u << op) - 1);
                hold >>= op;
                bits -= op;
            }
            if (bits < 15) {
                hold += (u32)(*in++) << bits;
                bits += 8;
                hold += (u32)(*in++) << bits;
                bits += 8;
            }
            here = dcode[hold & dmask];
        dodist:
            op = here.bits;
            hold >>= op;
            bits -= op;
            op = here.op;
            if (op & 16) {  // distance base
                dist = here.val;
                op &= 15;
                if (bits < op) {
                    hold += (u32)(*in++) << bits;
                    bits += 8;
                    if (bits < op) {
                        hold += (u32)(*in++) << bits;
                        bits += 8;
                    }
                }
                dist += hold & ((1u << op) - 1);
                hold >>= op;
                bits -= op;
                op = (u32)(out - beg);  // max distance in output
                if (dist > op) {        // copy from the window
                    op = dist - op;     // distance back in window
                    if (op > whave && sane) {
                        mem::w32(strm + 0x18, g_msg_too_far);
                        mem::w32(st, kModeBad);
                        break;
                    }
                    from = window;
                    if (wnext == 0) {  // very common case
                        from += wsize - op;
                        if (op < len) {
                            len -= op;
                            do *out++ = *from++;
                            while (--op);
                            from = out - dist;
                        }
                    } else if (wnext < op) {  // wrap around window
                        from += wsize + wnext - op;
                        op -= wnext;
                        if (op < len) {
                            len -= op;
                            do *out++ = *from++;
                            while (--op);
                            from = window;
                            if (wnext < len) {
                                op = wnext;
                                len -= op;
                                do *out++ = *from++;
                                while (--op);
                                from = out - dist;
                            }
                        }
                    } else {  // contiguous in window
                        from += wnext - op;
                        if (op < len) {
                            len -= op;
                            do *out++ = *from++;
                            while (--op);
                            from = out - dist;
                        }
                    }
                    while (len > 2) {
                        *out++ = *from++;
                        *out++ = *from++;
                        *out++ = *from++;
                        len -= 3;
                    }
                    if (len) {
                        *out++ = *from++;
                        if (len > 1) *out++ = *from++;
                    }
                } else {
                    from = out - dist;  // copy direct from output
                    do {                // minimum length is three
                        *out++ = *from++;
                        *out++ = *from++;
                        *out++ = *from++;
                        len -= 3;
                    } while (len > 2);
                    if (len) {
                        *out++ = *from++;
                        if (len > 1) *out++ = *from++;
                    }
                }
            } else if ((op & 64) == 0) {  // 2nd level distance code
                here = dcode[here.val + (hold & ((1u << op) - 1))];
                goto dodist;
            } else {
                mem::w32(strm + 0x18, g_msg_dist);
                mem::w32(st, kModeBad);
                break;
            }
        } else if ((op & 64) == 0) {  // 2nd level length code
            here = lcode[here.val + (hold & ((1u << op) - 1))];
            goto dolen;
        } else if (op & 32) {  // end of block
            mem::w32(st, kModeType);
            break;
        } else {
            mem::w32(strm + 0x18, g_msg_lit);
            mem::w32(st, kModeBad);
            break;
        }
    } while (in < last && out < end);

    // Return unused bytes (on entry, bits < 8, so in won't go too far back).
    len = bits >> 3;
    in -= len;
    bits -= len << 3;
    hold &= (1u << bits) - 1;

    mem::w32(strm, next_in + (u32)(in - in0));
    mem::w32(strm + 0xC, next_out + (u32)(out - out0));
    mem::w32(strm + 4, (u32)(in < last ? 5 + (last - in) : 5 - (in - last)));
    mem::w32(strm + 0x10, (u32)(out < end ? 257 + (end - out) : 257 - (out - end)));
    mem::w32(st + 0x38, hold);
    mem::w32(st + 0x3C, bits);
}
}  // namespace zfast

// NFSMW_VERIFY_NATIVE=1 (testing): run the native version and the game's own
// code on the same input and compare everything they write.
bool g_verify = false;
std::atomic<u64> g_verified{0}, g_mismatches{0};
void report_mismatch(const char* what, const char* detail) {
    if (g_mismatches.fetch_add(1) < 20) LOGE("native code: %s differs from the game's code: %s", what, detail);
}
void note_verified(const char* what) {
    static std::mutex mu;
    static std::vector<std::pair<const char*, u64>> counts;
    u64 n = 0;
    {
        std::lock_guard lk(mu);
        auto it = std::find_if(counts.begin(), counts.end(), [what](const auto& c) { return c.first == what; });
        if (it == counts.end()) counts.push_back({what, 0}), it = counts.end() - 1;
        n = ++it->second;
    }
    g_verified++;
    if ((n & (n - 1)) == 0 && n >= 256)
        LOGI("native code: %s: %llu calls verified so far, %llu mismatches in all", what, (unsigned long long)n,
             (unsigned long long)g_mismatches.load());
}

// The original function's first two instructions, then a jump to the rest
// (for verification: both must not depend on the pc).
u32 original_entry(u32 f) {
    return emit({mem::r32(f), mem::r32(f + 4), 0xE51FF004, f + 8});
}

bool install_inflate_fast(elf::Module* m) {
    const u32 f = m->base + 0x6674FC;
    // push {r4-r11,lr} ; add r11,sp,#28 ; sub sp,sp,#96 ; ldr r6,[r0,#0x1c] ; mov r12,r0 ; mov r5,#1 ;
    // mvn r4,#0 ; mvn r8,#0 ; ldr r0,[r6,#0x58] ; ldr r2,[r6,#0x54]
    if (!matches(f, {0xE92D4FF0, 0xE28DB01C, 0xE24DD060, 0xE590601C, 0xE1A0C000, 0xE3A05001, 0xE3E04000,
                     0xE3E08000, 0xE5960058, 0xE5962054}) ||
        !matches(f + 0x5FC, {0xE3A0000B}) || !matches(f + 0x630, {0xE3A0001D}) ||  // mode = TYPE (11), BAD (29)
        !matches(f + 0x94, {0xE2860D6F}))                                          // &state->sane (0x1BC0)
        return false;
    zfast::g_msg_too_far = mem::static_strdup("invalid distance too far back");
    zfast::g_msg_dist = mem::static_strdup("invalid distance code");
    zfast::g_msg_lit = mem::static_strdup("invalid literal/length code");
    if (!g_verify) {
        const u32 thunk = thunks::add("native_inflate_fast", [](cpu::GuestThread& t) {
            zfast::inflate_fast(t.regs()[0], t.regs()[1]);
        });
        jump(f, thunk);
        return true;
    }
    static u32 orig = original_entry(f);
    const u32 thunk = thunks::add(
        "native_inflate_fast_verify",
        [](cpu::GuestThread& t) {
            const u32 strm = t.regs()[0], start = t.regs()[1];
            const u32 st = mem::r32(strm + 0x1C);
            const u32 out = mem::r32(strm + 0xC);
            // Only bytes before next_out are read (earlier output), and both
            // versions write forward from next_out: no need to restore those.
            std::vector<u8> s0(mem::host(strm), mem::host(strm) + 0x38), st0(mem::host(st), mem::host(st) + 0x70);
            zfast::inflate_fast(strm, start);
            const u32 out1 = mem::r32(strm + 0xC);
            std::vector<u8> s1(mem::host(strm), mem::host(strm) + 0x38), st1(mem::host(st), mem::host(st) + 0x70),
                o1(mem::host(out), mem::host(out1));
            const u32 msg1 = mem::r32(strm + 0x18);
            std::memcpy(mem::host(strm), s0.data(), s0.size());
            std::memcpy(mem::host(st), st0.data(), st0.size());
            t.call(orig, {strm, start});
            const u32 out2 = mem::r32(strm + 0xC);
            std::vector<u8> s2(mem::host(strm), mem::host(strm) + 0x38), st2(mem::host(st), mem::host(st) + 0x70),
                o2(mem::host(out), mem::host(out2));
            const u32 msg2 = mem::r32(strm + 0x18);
            const bool msg_ok = msg1 == msg2 ||
                                (msg1 && msg2 && std::strcmp((const char*)mem::host(msg1), (const char*)mem::host(msg2)) == 0);
            std::memcpy(&s1[0x18], &s2[0x18], 4);  // compared above
            if (s1 != s2 || st1 != st2 || o1 != o2 || !msg_ok) {
                char d[160];
                std::snprintf(d, sizeof d, "stream %s, state %s, output %s, message %s", s1 != s2 ? "DIFF" : "ok",
                              st1 != st2 ? "DIFF" : "ok", o1 != o2 ? "DIFF" : "ok", msg_ok ? "ok" : "DIFF");
                report_mismatch("inflate_fast", d);
            }
            note_verified("inflate_fast");
        },
        true);
    jump(f, thunk);
    return true;
}

// ---- FNV-1 hash -----------------------------------------------------------------
// void hash(u32* h, const u8* p, int n): *h = (*h * 16777619) ^ byte, for each
// byte; the game hashes much of what it loads with it.
bool install_fnv1(elf::Module* m) {
    const u32 f = m->base + 0x4EDAD4;
    if (!matches(f, {0xE92D4800, 0xE1A0B00D, 0xE3520001, 0xB8BD8800, 0xE5903000, 0xE300C193, 0xE340C100, 0xE00E0C93,
                     0xE4D13001, 0xE2522001, 0xE02E3003, 0xE5803000, 0x1AFFFFF9, 0xE8BD8800}))
        return false;
    auto native = [](u32 hp, u32 buf, s32 n) -> u32 {
        u32 h = mem::r32(hp);
        if (n < 1) return h;
        if (!in_guest(buf, (u32)n)) fatal("fnv1: bad buffer %08X+%d", buf, n);
        const u8* p = mem::host(buf);
        for (s32 i = 0; i < n; i++) h = (h * 0x01000193u) ^ p[i];
        return h;
    };
    if (!g_verify) {
        const u32 thunk = thunks::add("native_fnv1", [native](cpu::GuestThread& t) {
            const u32 hp = t.regs()[0];
            const s32 n = (s32)t.regs()[2];
            if (n < 1) return;
            if (!in_guest(hp, 4)) fatal("fnv1: bad state %08X", hp);
            mem::w32(hp, native(hp, t.regs()[1], n));
        });
        jump(f, thunk);
        return true;
    }
    static u32 orig = original_entry(f);
    const u32 thunk = thunks::add(
        "native_fnv1_verify",
        [native](cpu::GuestThread& t) {
            const u32 hp = t.regs()[0], buf = t.regs()[1];
            const s32 n = (s32)t.regs()[2];
            const u32 want = native(hp, buf, n);
            t.call(orig, {hp, buf, (u32)n});
            if (mem::r32(hp) != want) report_mismatch("fnv1", "hash");
            note_verified("fnv1");
        },
        true);
    jump(f, thunk);
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
    g_verify = std::getenv("NFSMW_VERIFY_NATIVE") != nullptr;
    const bool crc = install_crc32(m), dec = install_decrypt(m), copy = install_copy(m);
    const bool only_026 = std::getenv("NFSMW_NATIVE_026") != nullptr;  // comparisons with 0.2.6
    const bool inf = !only_026 && install_inflate_fast(m), fnv = !only_026 && install_fnv1(m);
    LOGI("native code: crc32 %s, zip decryption %s, stored-data copy %s, inflate_fast %s, FNV-1 hash %s%s",
         crc ? "on" : "skipped", dec ? "on" : "skipped", copy ? "on" : "skipped", inf ? "on" : "skipped",
         fnv ? "on" : "skipped", g_verify ? " (verifying against the game's code)" : "");
}

}  // namespace game::native
