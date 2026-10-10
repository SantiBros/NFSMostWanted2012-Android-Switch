// Guest (32-bit ARM) address space.
//
// The whole 4 GiB guest address space is mirrored at a single host base
// address, so guest address A lives at host address g_base + A. This is what
// lets Dynarmic use "fastmem" (direct loads/stores) and lets host thunks turn
// guest pointers into host pointers with one addition.
#pragma once

#include "core/common.h"

namespace mem {

// Layout of the guest address space.
constexpr u32 NULL_GUARD_END = 0x00010000;  // [0, 64K) is never mapped
constexpr u32 RUNTIME_BASE = 0x00010000;    // small ARM runtime blob (traps, setjmp)
constexpr u32 THUNK_BASE = 0x00100000;      // one 8-byte stub per host function
constexpr u32 THUNK_END = 0x00200000;
constexpr u32 STATIC_BASE = 0x00200000;     // data owned by the HLE layer
constexpr u32 STATIC_END = 0x01000000;
constexpr u32 MODULE_BASE = 0x01000000;     // the game's .so files
constexpr u32 MODULE_END = 0x10000000;
constexpr u32 HEAP_BASE = 0x10000000;       // guest malloc / mmap / thread stacks
// The heap ends at the end of the guest window (platform dependent).

// Fixed addresses inside the runtime blob (see runtime_blob in memory.cpp).
constexpr u32 RT_RETURN_TRAP = RUNTIME_BASE + 0x00;
constexpr u32 RT_SETJMP = RUNTIME_BASE + 0x08;
constexpr u32 RT_LONGJMP = RUNTIME_BASE + 0x1C;
constexpr u32 RT_THREAD_EXIT = RUNTIME_BASE + 0x34;
constexpr u32 SVC_RETURN_TRAP = 0xFFFFFF;
constexpr u32 SVC_THREAD_EXIT = 0xFFFFFE;

extern u8* g_base;
extern u64 g_size;      // bytes of guest address space backed by host memory
extern bool g_fastmem;  // JIT accesses guest memory directly (else page table)

void init();

inline u8* host(u32 a) { return g_base + a; }
template <class T = void>
inline T* ptr(u32 a) {
    return a ? reinterpret_cast<T*>(g_base + a) : nullptr;
}
inline const char* str(u32 a) { return a ? reinterpret_cast<const char*>(g_base + a) : nullptr; }
inline u32 guest(const void* p) {
    return p ? static_cast<u32>(reinterpret_cast<const u8*>(p) - g_base) : 0;
}
inline bool is_guest(const void* p) {
    auto d = reinterpret_cast<const u8*>(p) - g_base;
    return d >= 0 && d < (1ll << 32);
}

inline u8 r8(u32 a) { return g_base[a]; }
inline u16 r16(u32 a) { u16 v; std::memcpy(&v, g_base + a, 2); return v; }
inline u32 r32(u32 a) { u32 v; std::memcpy(&v, g_base + a, 4); return v; }
inline u64 r64(u32 a) { u64 v; std::memcpy(&v, g_base + a, 8); return v; }
inline void w8(u32 a, u8 v) { g_base[a] = v; }
inline void w16(u32 a, u16 v) { std::memcpy(g_base + a, &v, 2); }
inline void w32(u32 a, u32 v) { std::memcpy(g_base + a, &v, 4); }
inline void w64(u32 a, u64 v) { std::memcpy(g_base + a, &v, 8); }

// Bump allocator for data that lives for the whole run (JNI tables, ctype
// tables, FILE structs for stdin/out/err, interned strings...).
u32 static_alloc(u32 size, u32 align = 8);
u32 static_strdup(const char* s);

// Guest heap (malloc family). Thread safe.
u32 heap_alloc(u32 size, u32 align = 16);
u32 heap_calloc(u32 n, u32 size);
u32 heap_realloc(u32 p, u32 size);
void heap_free(u32 p);
u32 heap_usable_size(u32 p);
u32 heap_strdup(const char* s);
u64 heap_bytes_in_use();

}  // namespace mem
