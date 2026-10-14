/* Compile-time statistics (NFS:MW Switch port addition).
 * Time spent translating and emitting new blocks, for the loader's log. */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace Dynarmic {

struct CompileStats {
    std::uint64_t ns = 0;      // time spent compiling blocks
    std::uint64_t blocks = 0;  // blocks compiled
};

// All threads together.
inline std::atomic<std::uint64_t> g_compile_ns{0};
inline std::atomic<std::uint64_t> g_compile_blocks{0};
// The calling thread (each Jit runs on one thread at a time).
inline thread_local CompileStats t_compile_stats;

// Called after each block is translated, with its location descriptor
// (IR::LocationDescriptor::Value()), on the thread that translated it.
inline void (*g_compile_hook)(std::uint64_t descriptor) = nullptr;

// Times one block compile: construct before translating, destroy after emitting.
class CompileTimer {
public:
    explicit CompileTimer(std::uint64_t descriptor) : descriptor_(descriptor), start_(std::chrono::steady_clock::now()) {}
    ~CompileTimer() {
        const auto ns = (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start_)
                            .count();
        g_compile_ns.fetch_add(ns, std::memory_order_relaxed);
        g_compile_blocks.fetch_add(1, std::memory_order_relaxed);
        t_compile_stats.ns += ns;
        t_compile_stats.blocks++;
        if (g_compile_hook) g_compile_hook(descriptor_);
    }

private:
    std::uint64_t descriptor_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace Dynarmic
