// Host functions exposed to the guest. Each function gets an 8-byte ARM stub
// in guest memory ("svc #index ; bx lr"); the SVC handler dispatches to the
// C++ implementation. Data symbols (e.g. __sF, _ctype_) map a name to a
// guest address directly.
#pragma once

#include <functional>
#include <string>

#include "core/abi.h"

namespace thunks {

using Fn = std::function<void(cpu::GuestThread&)>;

struct Entry {
    std::string name;
    Fn fn;
    bool reentrant = false;  // may call back into guest code
    u32 addr = 0;
    bool missing = false;
};

// Register a function. Re-entrant thunks run outside the JIT's Run() so they
// may call guest code (callbacks, JNI natives, pthread_once routines...).
u32 add(const std::string& name, Fn fn, bool reentrant = false);
void add_data(const std::string& name, u32 guest_addr);
// Guest address for a host symbol, or 0.
u32 lookup(const std::string& name);
// A stub that logs the call (once) and returns 0.
u32 make_missing(const std::string& name);

const Entry* entry(u32 index);
void dispatch(cpu::GuestThread& t, u32 index);
// Reverse lookup for crash reports: name of the thunk at a guest address.
const char* name_at(u32 addr);

}  // namespace thunks

#define REG(name, fn) ::thunks::add(name, ::abi::wrap(fn))
#define REG_RE(name, fn) ::thunks::add(name, ::abi::wrap(fn), true)
#define REG_RAW(name, fn) ::thunks::add(name, fn)
#define REG_RAW_RE(name, fn) ::thunks::add(name, fn, true)
