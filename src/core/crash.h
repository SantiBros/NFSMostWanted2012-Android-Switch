// Crash reports for the port itself.
//
// When the port crashes (a bad memory access in the loader, in the game code
// it translated, or an error it did not expect), the platform's handler fills
// a HostState and report() appends to log.txt: the log lines not written yet,
// where the crash happened (as an offset in nfsmw.elf, the build's symbols,
// kept with each release), and what the crashed thread was doing in the game.
// Before 0.2.8 such a crash ended the log up to a second early, with nothing
// about the crash itself.
//
// report() runs on the crashed thread: it allocates no memory and waits for no
// lock (the thread may hold one).
#pragma once

#include "core/common.h"

namespace crash {

struct HostState {
    const char* what = "";      // "bad memory access (data abort)"
    u64 pc = 0, lr = 0, sp = 0;
    bool has_address = false;   // fault_address is meaningful
    u64 fault_address = 0;
    u64 esr = 0;                // Switch: the exception syndrome
    u64 image_base = 0, image_end = 0;  // the port's code (for offsets)
    const char* image_name = "nfsmw.elf";
    const u64* gprs = nullptr;  // general registers, if known
    int ngprs = 0;
    u64 stack[16] = {};         // words on the stack that point into the port's code
    int nstack = 0;
};

// Set up the handlers: the platform's (CPU exceptions / signals) and one for
// C++ exceptions nobody caught. Call once the port's folder is known.
void install();

// Append the report to log.txt (see above).
void report(const HostState& s);

// Debugging aid: NFSMW_TEST_CRASH=<stage>:<kind> crashes on purpose at that
// point of the start-up (stages: libs, init, frame; kinds: host, guest,
// thread, throw, oom), to check the reports.
void test_point(const char* stage);

}  // namespace crash
