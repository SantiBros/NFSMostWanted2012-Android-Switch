#include "core/memory.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>

#include "platform/platform.h"

namespace mem {

u8* g_base = nullptr;
u64 g_size = 0;
bool g_fastmem = false;

namespace {

// Hand-assembled ARM (A32) code placed at RUNTIME_BASE. Assembled with
// arm-linux-gnueabi-as and verified with objdump; see docs/runtime_blob.S.
const u32 runtime_blob[] = {
    // 0x00 return trap: svc #0xFFFFFF ; b .-4
    0xEFFFFFFF, 0xEAFFFFFD,
    // 0x08 setjmp(r0=buf): mov ip,sp ; stmia r0!,{r4-r12,lr} ; vstmia r0,{d8-d15} ; mov r0,#0 ; bx lr
    0xE1A0C00D, 0xE8A05FF0, 0xEC808B10, 0xE3A00000, 0xE12FFF1E,
    // 0x1C longjmp(r0=buf,r1=val): ldmia r0!,{r4-r12,lr} ; mov sp,ip ; vldmia r0,{d8-d15} ;
    //                              movs r0,r1 ; moveq r0,#1 ; bx lr
    0xE8B05FF0, 0xE1A0D00C, 0xEC908B10, 0xE1B00001, 0x03A00001, 0xE12FFF1E,
    // 0x34 thread exit trap: svc #0xFFFFFE ; b .-4
    0xEFFFFFFE, 0xEAFFFFFD,
};

std::mutex g_static_mutex;
u32 g_static_next = STATIC_BASE;

// ---------------------------------------------------------------------------
// Guest heap
// ---------------------------------------------------------------------------
constexpr u32 HDR = 16;
constexpr u32 MAGIC = 0x6D616C63;  // 'malc'
constexpr u32 SMALL_STEP = 16;
constexpr u32 SMALL_MAX = 2048;
constexpr u32 NUM_CLASSES = SMALL_MAX / SMALL_STEP;  // class i holds (i+1)*16 bytes
constexpr u32 KIND_LARGE = 0xFFFF;
constexpr u32 CHUNK = 256 * 1024;

struct Header {
    u32 magic;
    u32 kind;
    u32 block_start;
    u32 block_size;
};

std::mutex g_heap_mutex;
std::map<u32, u32> g_free_by_addr;          // start -> size
std::multimap<u32, u32> g_free_by_size;     // size -> start
u32 g_class_free[NUM_CLASSES];
u32 g_class_chunk_next[NUM_CLASSES];
u32 g_class_chunk_end[NUM_CLASSES];
u64 g_in_use = 0;

Header* hdr(u32 user) { return reinterpret_cast<Header*>(g_base + user - HDR); }

void free_insert(u32 start, u32 size) {
    // coalesce with neighbours
    auto next = g_free_by_addr.lower_bound(start);
    if (next != g_free_by_addr.end() && start + size == next->first) {
        auto range = g_free_by_size.equal_range(next->second);
        for (auto it = range.first; it != range.second; ++it)
            if (it->second == next->first) { g_free_by_size.erase(it); break; }
        size += next->second;
        next = g_free_by_addr.erase(next);
    }
    if (next != g_free_by_addr.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == start) {
            auto range = g_free_by_size.equal_range(prev->second);
            for (auto it = range.first; it != range.second; ++it)
                if (it->second == prev->first) { g_free_by_size.erase(it); break; }
            start = prev->first;
            size += prev->second;
            g_free_by_addr.erase(prev);
        }
    }
    g_free_by_addr[start] = size;
    g_free_by_size.emplace(size, start);
}

// Returns a raw region of exactly `size` bytes (size multiple of 16) aligned to `align`.
u32 raw_alloc(u32 size, u32 align) {
    auto it = g_free_by_size.lower_bound(size);
    for (; it != g_free_by_size.end(); ++it) {
        u32 start = it->second, fsize = it->first;
        u32 aligned = align_up(start, align);
        if (aligned - start + size <= fsize) {
            g_free_by_size.erase(it);
            g_free_by_addr.erase(start);
            if (aligned > start) free_insert(start, aligned - start);
            u32 tail = start + fsize - (aligned + size);
            if (tail) free_insert(aligned + size, tail);
            return aligned;
        }
    }
    return 0;
}

}  // namespace

void init() {
    platform::GuestSpace gs = platform::reserve_guest_space();
    g_base = static_cast<u8*>(gs.base);
    g_size = gs.size;
    g_fastmem = gs.fastmem;
    if (!g_base || g_size < HEAP_BASE + (64u << 20)) fatal("could not reserve the guest address space");
    // The game expects the memory of a new process: zero. On the Switch the
    // guest space comes from the heap, which still holds whatever the program
    // that ran before in the same Homebrew loader session left there (the
    // Homebrew Menu itself, a forwarder...). Before 0.2.8 that reached the
    // libraries' zero-initialised data (.bss): with leftovers there, libc++'s
    // start-up code (std::locale) followed a garbage pointer and the port
    // crashed before the game started, on some consoles only.
    if (gs.dirty > NULL_GUARD_END) {
        const auto t0 = std::chrono::steady_clock::now();
        std::memset(g_base + NULL_GUARD_END, 0, std::min<u64>(gs.dirty, g_size) - NULL_GUARD_END);
        LOGI("guest memory cleared: %llu MiB in %.0f ms", (unsigned long long)(std::min<u64>(gs.dirty, g_size) >> 20),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::memcpy(g_base + RUNTIME_BASE, runtime_blob, sizeof(runtime_blob));
    u64 heap_end = std::min<u64>(g_size, 0xFF000000ull);
    free_insert(HEAP_BASE, (u32)(heap_end - HEAP_BASE));
    LOGI("guest address space at host %p, %llu MiB, %s", (void*)g_base, (unsigned long long)(g_size >> 20),
         g_fastmem ? "fastmem" : "page table");
}

u32 static_alloc(u32 size, u32 align) {
    std::lock_guard lk(g_static_mutex);
    u32 a = align_up(g_static_next, align);
    if (a + size > STATIC_END) fatal("static guest area exhausted");
    g_static_next = a + size;
    std::memset(g_base + a, 0, size);
    return a;
}

u32 static_strdup(const char* s) {
    u32 n = (u32)std::strlen(s) + 1;
    u32 a = static_alloc(n, 1);
    std::memcpy(g_base + a, s, n);
    return a;
}

u32 heap_alloc(u32 size, u32 align) {
    if (size == 0) size = 1;
    if (size > 0xC0000000u) return 0;
    std::lock_guard lk(g_heap_mutex);
    if (align <= 16 && size <= SMALL_MAX) {
        u32 cls = (size + SMALL_STEP - 1) / SMALL_STEP - 1;
        u32 csize = (cls + 1) * SMALL_STEP;
        u32 user = g_class_free[cls];
        if (user) {
            g_class_free[cls] = r32(user);
        } else {
            u32 need = HDR + csize;
            if (g_class_chunk_next[cls] + need > g_class_chunk_end[cls] || !g_class_chunk_next[cls]) {
                u32 c = raw_alloc(CHUNK, 16);
                if (!c) return 0;
                g_class_chunk_next[cls] = c;
                g_class_chunk_end[cls] = c + CHUNK;
            }
            u32 block = g_class_chunk_next[cls];
            g_class_chunk_next[cls] += need;
            user = block + HDR;
            Header* h = hdr(user);
            h->magic = MAGIC;
            h->kind = cls;
            h->block_start = block;
            h->block_size = need;
        }
        g_in_use += csize;
        return user;
    }
    if (align < 16) align = 16;
    u32 body = align_up(size, 16);
    u32 total = body + HDR + (align > 16 ? align : 0);
    total = align_up(total, 16);
    u32 block = raw_alloc(total, 16);
    if (!block) {
        LOGE("guest heap: out of memory (%u bytes requested)", size);
        return 0;
    }
    u32 user = align_up(block + HDR, align);
    Header* h = hdr(user);
    h->magic = MAGIC;
    h->kind = KIND_LARGE;
    h->block_start = block;
    h->block_size = total;
    g_in_use += total;
    return user;
}

u32 heap_calloc(u32 n, u32 size) {
    u64 t = (u64)n * size;
    if (t > 0xC0000000u) return 0;
    u32 p = heap_alloc((u32)t);
    if (p) std::memset(g_base + p, 0, (size_t)t);
    return p;
}

u32 heap_usable_size(u32 p) {
    if (!p) return 0;
    Header* h = hdr(p);
    if (h->magic != MAGIC) return 0;
    if (h->kind == KIND_LARGE) return h->block_start + h->block_size - p;
    return (h->kind + 1) * SMALL_STEP;
}

void heap_free(u32 p) {
    if (!p) return;
    if (p < HEAP_BASE || p >= g_size) {
        LOGW("free() of non-heap pointer 0x%08X ignored", p);
        return;
    }
    std::lock_guard lk(g_heap_mutex);
    Header* h = hdr(p);
    if (h->magic != MAGIC) {
        LOGW("free() of invalid pointer 0x%08X ignored", p);
        return;
    }
    if (h->kind == KIND_LARGE) {
        u32 start = h->block_start, size = h->block_size;
        h->magic = 0;
        g_in_use -= size;
        free_insert(start, size);
    } else {
        u32 cls = h->kind;
        w32(p, g_class_free[cls]);
        g_class_free[cls] = p;
        g_in_use -= (cls + 1) * SMALL_STEP;
    }
}

u32 heap_realloc(u32 p, u32 size) {
    if (!p) return heap_alloc(size);
    if (size == 0) {
        heap_free(p);
        return 0;
    }
    u32 old = heap_usable_size(p);
    if (old == 0) {
        LOGW("realloc() of invalid pointer 0x%08X", p);
        return 0;
    }
    if (size <= old && (old <= SMALL_MAX || size > old / 2)) return p;
    u32 n = heap_alloc(size);
    if (!n) return 0;
    std::memcpy(g_base + n, g_base + p, old < size ? old : size);
    heap_free(p);
    return n;
}

u32 heap_strdup(const char* s) {
    u32 n = (u32)std::strlen(s) + 1;
    u32 a = heap_alloc(n);
    std::memcpy(g_base + a, s, n);
    return a;
}

u64 heap_bytes_in_use() { return g_in_use; }

}  // namespace mem
