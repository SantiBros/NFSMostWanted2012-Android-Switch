#include <chrono>
#include <mutex>

#include "core/common.h"
#include "game/paths.h"
#include "platform/platform.h"

namespace logging {

#if defined(__SWITCH__)
// The log goes to the SD card: keep it to the useful lines.
int g_min_level = Info;
#else
int g_min_level = Debug;
#endif
bool g_to_stderr = true;
static std::mutex g_mutex;
static FILE* g_file = nullptr;
static const auto g_start = std::chrono::steady_clock::now();

void vwrite(Level lvl, const char* tag, const char* fmt, va_list ap) {
    if (lvl < g_min_level) return;
    static const char kL[] = {'V', 'D', 'I', 'W', 'E'};
    char msg[4096];
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    std::lock_guard lk(g_mutex);
    if (!g_file) {
        g_file = std::fopen(game::paths::log_file().c_str(), "w");
    }
    if (g_to_stderr) std::fprintf(stderr, "%9.3f %c/%s: %s\n", t, kL[lvl], tag, msg);
    if (g_file) {
        std::fprintf(g_file, "%9.3f %c/%s: %s\n", t, kL[lvl], tag, msg);
        std::fflush(g_file);
    }
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
    std::fflush(nullptr);
    platform::show_fatal(
        (std::string(msg) + "\n\nDetails are in " + game::paths::display(game::paths::log_file())).c_str());
    std::_Exit(1);
}
