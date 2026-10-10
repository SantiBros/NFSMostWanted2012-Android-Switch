// Common types, logging and small helpers shared by the whole loader.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

namespace logging {
enum Level { Verbose = 0, Debug, Info, Warn, Error };
extern int g_min_level;
extern bool g_to_stderr;  // off while a text screen shows stderr (Switch console)
void write(Level lvl, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void vwrite(Level lvl, const char* tag, const char* fmt, va_list ap);
// Write the lines logged so far to the log file now (they are otherwise
// written by a background thread, at least once a second).
void flush();
}  // namespace logging

#define LOGV(...) ::logging::write(::logging::Verbose, "loader", __VA_ARGS__)
#define LOGD(...) ::logging::write(::logging::Debug, "loader", __VA_ARGS__)
#define LOGI(...) ::logging::write(::logging::Info, "loader", __VA_ARGS__)
#define LOGW(...) ::logging::write(::logging::Warn, "loader", __VA_ARGS__)
#define LOGE(...) ::logging::write(::logging::Error, "loader", __VA_ARGS__)

[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

template <class To, class From>
inline To bit_cast_(const From& f) {
    static_assert(sizeof(To) == sizeof(From));
    To t;
    std::memcpy(&t, &f, sizeof(To));
    return t;
}

inline u32 align_up(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }
