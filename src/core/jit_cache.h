// JIT cache: which parts of the game's code were translated, remembered from
// one session to the next, and translated ahead of time.
//
// The game's ARM code is translated block by block the first time it runs.
// That is a large part of every loading screen (the game starts a new thread
// for each one, and a new thread used to mean a new, empty JIT) and the cause
// of a stutter whenever the game runs code for the first time in a race (a
// police chase starting, a crash camera...). The JIT cache records the
// location of every block that gets translated, in cache/ in the port's
// folder, and in later sessions:
//  - translates the loading threads' blocks into a spare JIT in the
//    background, which the next loading thread takes over;
//  - translates the main thread's blocks while the main thread would
//    otherwise wait for the GPU, so new code in a race is usually ready.
// The lists are tied to the exact game libraries and port version; anything
// else starts a new list. Only addresses are stored, never game code.
#pragma once

#include <string>

#include "core/common.h"

namespace jit_cache {

// After the game's libraries are loaded and relocated, before the loader
// patches anything: fingerprint the game's code (the lists belong to it).
void fingerprint();
// After the game's libraries are patched, before any game code runs: load the
// lists for this game build and start recording.
void init(const std::string& dir);
// Start translating the loading threads' list into a spare JIT (background).
void start_worker_warmup();
// From now on the spare JIT may go to a new guest thread (call once the game
// has started its long-lived service threads, i.e. after its first frames).
void spare_may_go();
// Main thread, while it waits for the GL thread: translate one more block of
// the main thread's list. Returns false when there is nothing left to do.
bool main_idle_step();
// Write the lists now (exit paths); otherwise a background thread writes them
// when they have grown.
void save();
// "JIT cache: ..." for the log, or "".
std::string summary();

}  // namespace jit_cache
