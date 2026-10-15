// CRC-32 as zlib computes it (reflected polynomial 0xEDB88320): crc32(0, p, n)
// gives a file's CRC as listed in a zip archive. Uses the CPU's CRC-32
// instructions where there are some (the Switch), else slicing-by-8 tables.
#pragma once

#include <cstddef>

#include "core/common.h"

namespace checksum {

u32 crc32(u32 crc, const void* data, size_t n);

}  // namespace checksum
