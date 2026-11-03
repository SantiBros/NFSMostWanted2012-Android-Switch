// Guest-side versions of the tiniest C library functions (0.3.1).
//
// Every C library function the game calls is a host function: the translated
// code leaves the JIT (an SVC), the call is dispatched to C++, and the JIT is
// entered again. For most calls that cost does not matter, but a race load
// calls strcmp about 3.5 million times and __aeabi_memclr8 about 2.7 million
// times (desktop build, log.txt "host calls" line of the loading thread), each
// on a few bytes, and strlen, tolower and toupper half a million times or more.
// Those run here as small ARM routines in guest memory
// instead, translated like the game's own code, with no host call at all; a
// large clear still goes to the host's memset. NFSMW_NO_GUEST_FAST=1 turns
// them off (for comparisons).
#include <cstdlib>
#include <vector>

#include "core/common.h"
#include "core/memory.h"
#include "core/thunks.h"
#include "hle/hle.h"

namespace hle {

namespace {
u32 emit(const std::vector<u32>& code) {
    const u32 at = mem::static_alloc((u32)code.size() * 4, 16);
    for (size_t i = 0; i < code.size(); i++) mem::w32(at + (u32)i * 4, code[i]);
    return at;
}
}  // namespace

void install_guest_fast() {
    if (std::getenv("NFSMW_NO_GUEST_FAST")) {
        LOGI("guest C library routines: off (NFSMW_NO_GUEST_FAST)");
        return;
    }
    // int strcmp(const char* a, const char* b): bytes compared as unsigned.
    const u32 strcmp_at = emit({
        0xE4D02001,  // 1: ldrb r2, [r0], #1
        0xE4D13001,  //    ldrb r3, [r1], #1
        0xE3520001,  //    cmp r2, #1          (C clear when r2 == 0)
        0x21520003,  //    cmpcs r2, r3
        0x0AFFFFFA,  //    beq 1b
        0xE0420003,  //    sub r0, r2, r3
        0xE12FFF1E,  //    bx lr
    });

    // void __aeabi_memclr4/8(void* dest, size_t n), dest 4-byte aligned. Up
    // to 256 bytes here, 8 at a time, then the remainder; larger clears go to
    // the host function (its stub, with r0/r1 untouched).
    const u32 host_clr = thunks::lookup("__aeabi_memclr");
    if (!host_clr) {
        LOGW("guest C library routines: __aeabi_memclr not registered; left as host calls");
        return;
    }
    const u32 memclr_at = emit({
        0xE3510C01,  //    cmp r1, #256
        0x8A00000E,  //    bhi host                    (index 17)
        0xE3A02000,  //    mov r2, #0
        0xE3A03000,  //    mov r3, #0
        0xE2511008,  // 1: subs r1, r1, #8
        0xA8A0000C,  //    stmiage r0!, {r2, r3}
        0xAAFFFFFC,  //    bge 1b
        0xE2911008,  //    adds r1, r1, #8            (0..7 left)
        0x012FFF1E,  //    bxeq lr
        0xE3110004,  //    tst r1, #4
        0x14802004,  //    strne r2, [r0], #4
        0xE3110002,  //    tst r1, #2
        0x10C020B2,  //    strhne r2, [r0], #2
        0xE3110001,  //    tst r1, #1
        0x15C02000,  //    strbne r2, [r0]
        0xE12FFF1E,  //    bx lr
        0xE1A00000,  //    nop
        0xE51FF004,  // host: ldr pc, [pc, #-4]
        host_clr,    //    .word host stub
    });

    // size_t strlen(const char* s)
    const u32 strlen_at = emit({
        0xE1A01000,  //    mov r1, r0
        0xE4D12001,  // 1: ldrb r2, [r1], #1
        0xE3520000,  //    cmp r2, #0
        0x1AFFFFFC,  //    bne 1b
        0xE0410000,  //    sub r0, r1, r0
        0xE2400001,  //    sub r0, r0, #1
        0xE12FFF1E,  //    bx lr
    });
    // int tolower(int c) / toupper(int c), ASCII only (as the host functions).
    const u32 tolower_at = emit({
        0xE2401041,  //    sub r1, r0, #'A'
        0xE3510019,  //    cmp r1, #25
        0x92800020,  //    addls r0, r0, #32
        0xE12FFF1E,  //    bx lr
    });
    const u32 toupper_at = emit({
        0xE2401061,  //    sub r1, r0, #'a'
        0xE3510019,  //    cmp r1, #25
        0x92400020,  //    subls r0, r0, #32
        0xE12FFF1E,  //    bx lr
    });

    thunks::add_data("strcmp", strcmp_at);
    thunks::add_data("__aeabi_memclr4", memclr_at);
    thunks::add_data("__aeabi_memclr8", memclr_at);
    thunks::add_data("strlen", strlen_at);
    thunks::add_data("tolower", tolower_at);
    thunks::add_data("toupper", toupper_at);
    LOGI("guest C library routines: strcmp, strlen, tolower, toupper, __aeabi_memclr4/8 run as guest code");
}

}  // namespace hle
