#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#if defined(__SWITCH__)
#include <switch.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

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
std::mutex g_io_mutex;  // one writer at a time, lines stay in order
const auto g_start = std::chrono::steady_clock::now();
constexpr size_t kWakeAt = 16 * 1024;  // pending bytes that wake the writer early

// The log file. It is written without the C library's FILE (a crash report
// must be able to write to it while another thread is in the middle of a
// write): on the Switch straight through the SD card's file system, else with
// write(2). Opened once the port's folder is known.
std::atomic<bool> g_open{false}, g_closed{false};
#if defined(__SWITCH__)
FsFile g_file;
std::atomic<s64> g_offset{0};
bool open_one(const std::string& path) {
    FsFileSystem* fs = nullptr;
    char fs_path[FS_MAX_PATH] = {};
    if (fsdevTranslatePath(path.c_str(), &fs, fs_path) == -1 || !fs) return false;
    fsFsDeleteFile(fs, fs_path);  // a new log for every start
    fsFsCreateFile(fs, fs_path, 0, 0);
    if (R_FAILED(fsFsOpenFile(fs, fs_path, FsOpenMode_Write | FsOpenMode_Append, &g_file))) return false;
    fsFileSetSize(&g_file, 0);
    return true;
}
bool open_file(const std::string& path) {
    // log.txt can still be open from an earlier run in the same Homebrew
    // loader session (0.2.7 and older never closed it): then log-2.txt.
    if (open_one(path)) return true;
    const size_t dot = path.rfind('.');
    return dot != std::string::npos && open_one(path.substr(0, dot) + "-2" + path.substr(dot));
}
void write_file(const char* data, size_t n, bool sync) {
    if (!n) return;
    const s64 at = g_offset.fetch_add((s64)n);
    fsFileWrite(&g_file, at, data, n, sync ? FsWriteOption_Flush : FsWriteOption_None);
}
// Close it before the program ends: the Homebrew loader runs the next program
// in the same process, where a file left open could not be written again.
void close_file() { fsFileClose(&g_file); }
#else
int g_fd = -1;
bool open_file(const std::string& path) {
    g_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    return g_fd >= 0;
}
void write_file(const char* data, size_t n, bool) {
    while (n) {
        const ssize_t w = ::write(g_fd, data, n);
        if (w <= 0) return;
        data += w;
        n -= (size_t)w;
    }
}
void close_file() { ::close(g_fd); }
#endif

// Caller holds g_io_mutex (or the process is crashing).
bool ensure_open() {
    if (g_open.load()) return true;
    if (g_closed.load()) return false;
    const std::string path = game::paths::root().empty() ? std::string() : game::paths::log_file();
    if (path.empty() || !open_file(path)) return false;
    g_open = true;
    return true;
}

void writer_thread() {
    platform::set_thread_name("log");
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
    if (!ensure_open()) return;  // keep the lines until the file can be opened
    std::string text;
    {
        std::lock_guard lk(g_mutex);
        text.swap(g_pending);
    }
    write_file(text.data(), text.size(), false);
}

void close() {
    flush();
    std::lock_guard io(g_io_mutex);
    if (g_open.exchange(false)) close_file();
    g_closed = true;  // lines logged from now on are not written
}

void emergency_write(const char* text, size_t n) {
    // The crashed thread may hold a lock: take none that could block.
    if (!g_open.load()) {
        std::unique_lock io(g_io_mutex, std::try_to_lock);
        ensure_open();
    }
    if (!g_open.load()) return;
    std::unique_lock lk(g_mutex, std::try_to_lock);
    if (lk.owns_lock()) {
        write_file(g_pending.data(), g_pending.size(), false);
        g_pending.clear();  // keeps its memory: no allocation here
    }
    write_file(text, n, true);
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
        {
            std::lock_guard io(g_io_mutex);
            ensure_open();  // now, so that a crash report can always be written
        }
        std::atexit([] { close(); });
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
    logging::close();
    std::fflush(nullptr);
    platform::show_fatal(
        (std::string(msg) + "\n\nDetails are in " + game::paths::display(game::paths::log_file())).c_str());
    std::_Exit(1);
}
