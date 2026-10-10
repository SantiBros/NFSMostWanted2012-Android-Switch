// A read cache for the game's data file (the OBB).
//
// The game reads the OBB through stdio in thousands of small pieces: in a
// race its main thread alone does bursts of 200-300 reads of 4-12 KiB a
// second, each after a seek, and two streaming threads do more. On the Switch
// every one of those became a request to the SD card (newlib's FILE buffer is
// only 1 KiB, so a 4 KiB read is four of them), and the waiting showed up as
// hitches while the frame counter still said 30 fps.
//
// open() returns a FILE* whose reads come from a shared cache of 128 KiB
// blocks kept in memory (least recently used blocks are dropped when it is
// full). A miss reads one whole block from the card; everything around it is
// then served from memory. When a stream reads forward from one block into the
// next, a background thread reads the following blocks ahead of it (more the
// longer it keeps reading forward, up to 1 MiB), so a game thread loading a
// large piece of data mostly finds it already in memory. The
// game's stdio calls (fread, fseek, ftell, fgetc, ...) work on that FILE*
// unchanged.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace hle::filecache {

// A read-only FILE* for host_path that reads through the cache, or nullptr
// (the caller then opens the file normally).
FILE* open(const std::string& host_path);

struct Stats {
    uint64_t reads = 0;        // block lookups (reads within a stream's current block are not counted)
    uint64_t block_loads = 0;  // blocks read from the card (by the game's threads or ahead of them)
    uint64_t read_ahead = 0;   // of those, read ahead by the background thread
    uint64_t bytes_loaded = 0;
    uint64_t load_ns = 0;      // time spent reading blocks from the card
    uint64_t wait_ns = 0;      // time the game's threads waited for a block (what read-ahead saves)
    uint64_t cached_bytes = 0;
};
Stats stats();
// "OBB cache: 74 MiB, 2310 block lookups (97% in memory), 69 card reads (8.6 MiB, 40 ahead, 120 ms),
// game waited 60 ms" for the log.
std::string summary();

}  // namespace hle::filecache
