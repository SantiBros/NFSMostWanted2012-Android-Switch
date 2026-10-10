// Host threads with an explicit stack size (guest threads need more host stack
// than some platform defaults give, e.g. libnx's 128 KiB).
#pragma once

#include <functional>

namespace platform {

struct ThreadHandle;
// Starts fn on a new host thread. `core` is a hint for the Switch scheduler
// (-1 = any). Returns a handle that can be joined (or nullptr on failure).
ThreadHandle* spawn_thread(std::function<void()> fn, size_t stack_size = 1 << 20, int core = -1);
void join_thread(ThreadHandle* t);    // waits and frees the handle
// Raise the calling thread's priority above the game's worker threads
// (Switch: so the GL thread is not held up by them on its core).
void boost_current_thread();
// Keep the game's worker threads off this core from now on (Switch: the GL
// thread gets core 2 to itself; the game's threads then use core 1).
void reserve_core_for_gl(int core);
void detach_thread(ThreadHandle* t);  // frees the handle without waiting

}  // namespace platform
