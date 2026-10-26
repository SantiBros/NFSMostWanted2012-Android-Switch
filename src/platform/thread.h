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
// (Switch: so the GL thread and the sound mixer are not held up by them on
// their cores).
void boost_current_thread();
// Keep the game's worker threads off this core from now on (Switch: the GL
// thread gets core 2 to itself; the game's threads then use core 1).
void reserve_core_for_gl(int core);
// Frees the handle without waiting; the thread itself is released (joined)
// once it has finished, at the next spawn_thread or detach_thread (libnx has
// no pthread_detach, and the Switch allows an application 96 threads).
void detach_thread(ThreadHandle* t);
// Threads started and not yet released, the main thread included.
int live_threads();

// CPU time the calling thread has used so far, in nanoseconds.
unsigned long long thread_cpu_ns();
// Name the calling thread (shows in debuggers and the desktop's process tools).
void set_thread_name(const char* name);

}  // namespace platform
