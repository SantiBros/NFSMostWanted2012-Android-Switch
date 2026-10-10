// Hooking guest (ARM) code: small trampolines written into guest memory that
// call a host function before or after the original. Host functions run as
// ordinary (non re-entrant) thunks, so they must not call guest code.
#pragma once

#include "core/common.h"

namespace hook {

// Trampoline: calls `original` with r0-r3, then `host_stub` with
// r0 = result, r1 = first argument, r2 = second argument; returns the result.
// For functions with at most 4 word arguments.
u32 post(u32 original, u32 host_stub);
// Trampoline: calls `host_stub` with the original r0-r3, then jumps to
// `original` with all arguments (stack included) untouched.
u32 pre(u32 original, u32 host_stub);

// Wrap a (non-virtual) function in place: its first instruction is moved to
// a trampoline and replaced by a branch. Only works when the function starts
// with "push {...}"; returns false (and changes nothing) otherwise.
bool inline_post(u32 func, u32 host_stub);
bool inline_pre(u32 func, u32 host_stub);

}  // namespace hook
