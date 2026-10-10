#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "core/common.h"
#include "game/paths.h"
#include "platform/platform.h"
#include "platform/thread.h"

namespace logging {

#if defined(__SWITCH__)
// The log goes to the SD card: keep it to the useful lines.
int g_min_level = Info;
#else
int g_min_level = Debug;
#endif
bool g_to_stderr = true;

// Lines are collected in memory and written to the card by a background
// thread (at least once a second), never by the thread that logs: a write to
// the SD card can take several milliseconds, and the game logs from its main
// thread (0.2.5 wrote and flushed every line there, which showed as hitches
// whenever the game printed a burst of messages). Errors, fatal() and the
// exit paths flush at once.
namespace {
std::mutex g_mutex;     // guards g_pending
std::string g_pending;  // lines not written yet
std::condition_variable g_wake;
bool g_writer = false;
std::mutex g_io_mutex;  // guards g_file: one writer at a time, lines stay in order
FILE* g_file = nullptr;
const auto g_start = std::chrono::steady_clock::now();
constexpr size_t kWakeAt = 16 * 1024;  // pending bytes that wake the writer early

void writer_thread() {
    for (;;) {
        {
            std::unique_lock lk(g_mutex);
            g_wake.wait_for(lk, std::chrono::seconds(1), [] { return g_pending.size() >= kWakeAt; });
        }
        flush();
    }
}
}  // namespace

void flush() {
    std::lock_guard io(g_io_mutex);
    std::string text;
    {
        std::lock_guard lk(g_mutex);
        text.swap(g_pending);
    }
    if (!g_file) g_file = std::fopen(game::paths::log_file().c_str(), "w");
    if (g_file && !text.empty()) {
        std::fwrite(text.data(), 1, text.size(), g_file);
        std::fflush(g_file);
    }
}

void vwrite(Level lvl, const char* tag, const char* fmt, va_list ap) {
    if (lvl < g_min_level) return;
    static const char kL[] = {'V', 'D', 'I', 'W', 'E'};
    char msg[4096];
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    char line[4200];
    const int n = std::snprintf(line, sizeof line, "%9.3f %c/%s: %s\n", t, kL[lvl], tag, msg);
    bool start_writer = false;
    {
        std::lock_guard lk(g_mutex);
        if (g_to_stderr) std::fputs(line, stderr);
        g_pending.append(line, (size_t)std::min<int>(n, (int)sizeof line - 1));
        if (!g_writer) {
            g_writer = true;
            start_writer = true;
        }
        if (g_pending.size() >= kWakeAt) g_wake.notify_one();
    }
    if (start_writer) {
        std::atexit(flush);
        platform::detach_thread(platform::spawn_thread(writer_thread, 128 * 1024));
    }
    if (lvl >= Error) flush();
}

void write(Level lvl, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwrite(lvl, tag, fmt, ap);
    va_end(ap);
}

}  // namespace logging

void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char msg[1024];
    va_list ap2;
    va_copy(ap2, ap);
    std::vsnprintf(msg, sizeof msg, fmt, ap2);
    va_end(ap2);
    logging::vwrite(logging::Error, "FATAL", fmt, ap);
    va_end(ap);
    logging::flush();
    std::fflush(nullptr);
    platform::show_fatal(
        (std::string(msg) + "\n\nDetails are in " + game::paths::display(game::paths::log_file())).c_str());
    std::_Exit(1);
}
