#include "core/crash.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <cstring>
#include <new>
#include <stdexcept>
#include <thread>

#include <dynarmic/interface/A32/a32.h>

#include "core/cpu.h"
#include "core/elf.h"
#include "core/memory.h"
#include "platform/platform.h"

#ifndef NFSMW_VERSION
#define NFSMW_VERSION "dev"
#endif

namespace crash {

namespace {
char g_text[12 * 1024];
size_t g_len = 0;

void add(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void add(const char* fmt, ...) {
    if (g_len + 1 >= sizeof g_text) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(g_text + g_len, sizeof g_text - g_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_len = std::min(sizeof g_text - 1, g_len + (size_t)n);
}

// "nfsmw.elf+0x1234" when the address is in the port's code.
void add_host_address(const HostState& s, u64 a) {
    if (s.image_end > s.image_base && a >= s.image_base && a < s.image_end)
        add("%s+0x%llX", s.image_name, (unsigned long long)(a - s.image_base));
    else
        add("0x%llX", (unsigned long long)a);
}

void add_guest_address(u32 a) {
    char buf[200];
    elf::describe_into(a, buf, sizeof buf);
    add("%s", buf);
}

void on_terminate() {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) std::_Exit(1);
    const char* what = "an unknown error";
    bool memory = false;
    char buf[256] = {};
    if (std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::bad_alloc&) {
            memory = true;
        } catch (const std::exception& x) {
            std::snprintf(buf, sizeof buf, "%s", x.what());
            what = buf;
        } catch (...) {
        }
    }
    HostState s;
    s.what = memory ? "out of memory (std::bad_alloc)" : "an error it did not expect (uncaught C++ exception)";
    report(s);
    if (memory) {
        u64 used = 0, total = 0;
        char numbers[96] = "";
        if (platform::memory_usage(used, total) && total >= used)
            std::snprintf(numbers, sizeof numbers, " (%llu MiB free of %llu MiB)", (unsigned long long)((total - used) >> 20),
                          (unsigned long long)(total >> 20));
        fatal("The port ran out of memory%s.\n\nStart the Homebrew Menu by holding R while you launch a game (not "
              "from the Album), so the port gets all of the memory a game gets. Please report this with log.txt.",
              numbers);
    }
    fatal("The port stopped on an error it did not expect:\n%s\n\nPlease report this with log.txt.", what);
}
}  // namespace

void install() {
    std::set_terminate(on_terminate);
    platform::install_crash_handler();
}

void report(const HostState& s) {
    g_len = 0;
    add("\n==================== CRASH ====================\n");
    add("NFS Most Wanted 2012 loader %s stopped: %s.\n", NFSMW_VERSION, s.what);
    if (s.pc) {
        add("  where:   ");
        add_host_address(s, s.pc);
        add("\n  caller:  ");
        add_host_address(s, s.lr);
        add("\n");
    }
    if (s.has_address)
        add("  address: 0x%llX%s\n", (unsigned long long)s.fault_address,
            s.fault_address < 0x10000 ? " (a null pointer)" : "");
    if (s.esr) add("  esr:     0x%08llX\n", (unsigned long long)s.esr);

    // What the crashed thread was doing in the game.
    cpu::GuestThread* t = cpu::current_or_null();
    if (!t) {
        add("  thread:  one of the port's own threads (graphics, sound, file reading, JIT cache or log)\n");
    } else {
        add("  thread:  game thread %u%s\n", t->id, t->is_main ? " (the game's main thread)" : "");
        if (const char* host_fn = t->in_thunk) {
            add("  in:      the port's version of %s, called from the game at ", host_fn);
            add_guest_address(t->in_thunk_lr);
            add("\n");
        } else if (t->call_depth == 0) {
            add("  game:    not in game code (the port's own work between game calls)\n");
        } else if (t->jit) {
            const auto& r = t->jit->Regs();
            add("  game:    running translated game code near ");
            add_guest_address(r[15]);
            add("\n           lr ");
            add_guest_address(r[14]);
            add("\n");
            for (int i = 0; i < 16; i += 4)
                add("           r%-2d=%08X r%-2d=%08X r%-2d=%08X r%-2d=%08X\n", i, r[i], i + 1, r[i + 1], i + 2,
                    r[i + 2], i + 3, r[i + 3]);
        }
    }
    if (s.nstack) {
        add("  stack:  ");
        for (int i = 0; i < s.nstack; i++) {
            add(" ");
            add_host_address(s, s.stack[i]);
        }
        add("\n");
    }
    for (int i = 0; i < s.ngprs; i += 4) {
        add("  ");
        for (int k = i; k < std::min(i + 4, s.ngprs); k++) add(" x%-2d=%016llX", k, (unsigned long long)s.gprs[k]);
        add("\n");
    }
    if (s.sp) add("   sp =%016llX\n", (unsigned long long)s.sp);
    add("Please report this: open an issue on GitHub and attach this whole log.txt.\n");
    add("===============================================\n");
    logging::emergency_write(g_text, g_len);
    if (logging::g_to_stderr) std::fwrite(g_text, 1, g_len, stderr);
}

void test_point(const char* stage) {
    static const char* const spec = std::getenv("NFSMW_TEST_CRASH");
    if (!spec) return;
    const char* colon = std::strchr(spec, ':');
    const size_t n = colon ? (size_t)(colon - spec) : std::strlen(spec);
    if (std::strlen(stage) != n || std::strncmp(spec, stage, n) != 0) return;
    const std::string kind = colon ? colon + 1 : "host";
    LOGW("NFSMW_TEST_CRASH: crashing on purpose (%s at %s)", kind.c_str(), stage);
    if (kind == "throw") throw std::runtime_error("test exception (NFSMW_TEST_CRASH)");
    if (kind == "oom") throw std::bad_alloc();
    if (kind == "thread") {
        std::thread([] { *static_cast<volatile int*>(nullptr) = 1; }).join();
        return;
    }
    if (kind == "guest" && cpu::current_or_null()) {
        // A guest store to an address outside guest memory: with direct memory
        // access (the Switch) this is a host fault inside translated code.
        const u32 code = mem::static_alloc(16, 16);
        mem::w32(code, 0xE3E0000F);      // mvn r0, #15     (r0 = 0xFFFFFFF0)
        mem::w32(code + 4, 0xE5800000);  // str r0, [r0]
        mem::w32(code + 8, 0xE12FFF1E);  // bx lr
        cpu::current().call(code, {});
        return;
    }
    *static_cast<volatile int*>(nullptr) = 1;
}

}  // namespace crash
