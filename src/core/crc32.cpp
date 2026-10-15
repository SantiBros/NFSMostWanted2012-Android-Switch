#include "core/crc32.h"

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define NFSMW_HW_CRC32 1
#endif

namespace checksum {

namespace {
#if !defined(NFSMW_HW_CRC32)
struct Tables {
    u32 t[8][256];
    Tables() {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (u32 i = 0; i < 256; i++)
            for (int k = 1; k < 8; k++) t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFF];
    }
};
const Tables& tables() {
    static const Tables t;  // thread-safe one-time initialisation
    return t;
}
#endif
}  // namespace

u32 crc32(u32 crc, const void* data, size_t n) {
    const u8* p = static_cast<const u8*>(data);
    crc = ~crc;
#if defined(NFSMW_HW_CRC32)
    while (n && ((uintptr_t)p & 7)) crc = __crc32b(crc, *p++), n--;
    for (; n >= 8; n -= 8, p += 8) {
        u64 v;
        std::memcpy(&v, p, 8);
        crc = __crc32d(crc, v);
    }
    while (n--) crc = __crc32b(crc, *p++);
#else
    const auto& t = tables().t;
    for (; n >= 8; n -= 8, p += 8) {
        u32 a, b;
        std::memcpy(&a, p, 4);
        std::memcpy(&b, p + 4, 4);
        a ^= crc;
        crc = t[7][a & 0xFF] ^ t[6][(a >> 8) & 0xFF] ^ t[5][(a >> 16) & 0xFF] ^ t[4][a >> 24] ^ t[3][b & 0xFF] ^
              t[2][(b >> 8) & 0xFF] ^ t[1][(b >> 16) & 0xFF] ^ t[0][b >> 24];
    }
    while (n--) crc = t[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
#endif
    return ~crc;
}

}  // namespace checksum
