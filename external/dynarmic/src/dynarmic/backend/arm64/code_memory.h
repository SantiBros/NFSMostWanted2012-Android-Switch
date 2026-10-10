/* Executable memory for the arm64 backend.
 *
 * nfsmw-port patch: Horizon (Nintendo Switch) never gives a process memory
 * that is writable and executable at the same address. Code is written
 * through one mapping (wptr) and executed from another (ptr) that alias the
 * same physical pages (libnx Jit / CodeMemory). The Linux build can use the
 * same dual-mapped scheme (DYNARMIC_DUAL_MAPPED_CODE) to test that path.
 * SPDX-License-Identifier: 0BSD
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>

#if defined(__SWITCH__)
#    include <switch.h>
#elif defined(DYNARMIC_DUAL_MAPPED_CODE)
#    include <oaknut/dual_code_block.hpp>
#else
#    include <oaknut/code_block.hpp>
#endif

namespace Dynarmic::Backend::Arm64 {

class CodeMemory {
public:
    explicit CodeMemory(std::size_t size)
#if defined(__SWITCH__)
    {
        size = (size + 0xFFF) & ~std::size_t(0xFFF);
        if (R_FAILED(jitCreate(&jit, size))) throw std::bad_alloc{};
        if (R_FAILED(jitTransitionToExecutable(&jit))) throw std::bad_alloc{};
        wmem = static_cast<std::uint32_t*>(jitGetRwAddr(&jit));
        xmem = static_cast<std::uint32_t*>(jitGetRxAddr(&jit));
        this->size = size;
    }
    ~CodeMemory() { jitClose(&jit); }
#else
        : block(size), size(size) {
#    if defined(DYNARMIC_DUAL_MAPPED_CODE)
        wmem = block.wptr();
        xmem = block.xptr();
#    else
        wmem = xmem = block.ptr();
#    endif
    }
#endif

    CodeMemory(const CodeMemory&) = delete;
    CodeMemory& operator=(const CodeMemory&) = delete;

    // Address code executes from (what all CodePtrs refer to).
    std::uint32_t* ptr() const { return xmem; }
    // Address code is written through.
    std::uint32_t* wptr() const { return wmem; }

    void protect() {
#if !defined(__SWITCH__) && !defined(DYNARMIC_DUAL_MAPPED_CODE)
        block.protect();
#endif
    }
    void unprotect() {
#if !defined(__SWITCH__) && !defined(DYNARMIC_DUAL_MAPPED_CODE)
        block.unprotect();
#endif
    }

    // `mem` is an execute-side address.
    void invalidate(std::uint32_t* mem, std::size_t bytes) {
#if defined(__SWITCH__)
        armDCacheFlush(wmem + (mem - xmem), bytes);
        armICacheInvalidate(mem, bytes);
#elif defined(DYNARMIC_DUAL_MAPPED_CODE)
        sync(wmem + (mem - xmem), mem, bytes);
#else
        block.invalidate(mem, bytes);
#endif
    }
    void invalidate_all() { invalidate(xmem, size); }
#if !defined(__SWITCH__) && !defined(DYNARMIC_DUAL_MAPPED_CODE)
    oaknut::CodeBlock& block_for_exception_handler() { return block; }
#endif

private:
#if defined(__SWITCH__)
    Jit jit{};
#elif defined(DYNARMIC_DUAL_MAPPED_CODE)
    static void sync(std::uint32_t* w, std::uint32_t* x, std::size_t bytes) {
#    if defined(__aarch64__)
        const std::uintptr_t line = 64;
        for (std::uintptr_t a = (std::uintptr_t)w & ~(line - 1); a < (std::uintptr_t)w + bytes; a += line)
            __asm__ volatile("dc cvau, %0" ::"r"(a) : "memory");
        __asm__ volatile("dsb ish" ::: "memory");
        for (std::uintptr_t a = (std::uintptr_t)x & ~(line - 1); a < (std::uintptr_t)x + bytes; a += line)
            __asm__ volatile("ic ivau, %0" ::"r"(a) : "memory");
        __asm__ volatile("dsb ish\nisb" ::: "memory");
#    else
        (void)w, (void)x, (void)bytes;
#    endif
    }
    oaknut::DualCodeBlock block;
#else
    oaknut::CodeBlock block;
#endif
    std::uint32_t* wmem = nullptr;
    std::uint32_t* xmem = nullptr;
    std::size_t size = 0;
};

}  // namespace Dynarmic::Backend::Arm64
