// stdio: printf/scanf formatting over guest arguments and FILE* emulation.
#include <unistd.h>

#include <cctype>
#include <mutex>
#include <unordered_map>

#include "hle/file_cache.h"
#include "hle/hle.h"

namespace hle {

namespace {
using mem::ptr;
using mem::str;

void append_utf8(std::string& out, u32 cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    }
}
}  // namespace

std::string format(const char* fmt, abi::VarArgs& va) {
    std::string out;
    if (!fmt) return "(null)";
    char tmp[512];
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            out += *p++;
            continue;
        }
        p++;
        if (*p == '%') {
            out += '%';
            p++;
            continue;
        }
        std::string spec = "%";
        while (*p && std::strchr("-+ #0'", *p)) spec += *p++;
        if (*p == '*') {
            spec += std::to_string((int)va.next32());
            p++;
        } else {
            while (std::isdigit((unsigned char)*p)) spec += *p++;
        }
        if (*p == '.') {
            spec += *p++;
            if (*p == '*') {
                spec += std::to_string((int)va.next32());
                p++;
            } else {
                while (std::isdigit((unsigned char)*p)) spec += *p++;
            }
        }
        enum { NONE, HH, H, L, LL, J, Z, T, BIGL } len = NONE;
        if (p[0] == 'h' && p[1] == 'h') { len = HH; p += 2; }
        else if (*p == 'h') { len = H; p++; }
        else if (p[0] == 'l' && p[1] == 'l') { len = LL; p += 2; }
        else if (*p == 'l') { len = L; p++; }
        else if (*p == 'q') { len = LL; p++; }
        else if (*p == 'j') { len = J; p++; }
        else if (*p == 'z') { len = Z; p++; }
        else if (*p == 't') { len = T; p++; }
        else if (*p == 'L') { len = BIGL; p++; }
        char conv = *p;
        if (!conv) break;
        p++;
        bool wide64 = (len == LL || len == J);
        switch (conv) {
        case 'd':
        case 'i': {
            long long v;
            if (wide64) v = (s64)va.next64();
            else {
                s32 x = (s32)va.next32();
                v = len == HH ? (s8)x : len == H ? (s16)x : x;
            }
            std::snprintf(tmp, sizeof tmp, (spec + "lld").c_str(), v);
            out += tmp;
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            unsigned long long v;
            if (wide64) v = va.next64();
            else {
                u32 x = va.next32();
                v = len == HH ? (u8)x : len == H ? (u16)x : x;
            }
            std::snprintf(tmp, sizeof tmp, (spec + "ll" + conv).c_str(), v);
            out += tmp;
            break;
        }
        case 'c': {
            u32 c = va.next32();
            if (len == L) {
                std::string s;
                append_utf8(s, c);
                out += s;
            } else {
                std::snprintf(tmp, sizeof tmp, (spec + "c").c_str(), (int)(u8)c);
                out += tmp;
            }
            break;
        }
        case 's': {
            u32 a = va.next32();
            std::string s;
            if (!a) s = "(null)";
            else if (len == L) {
                for (u32 q = a; mem::r32(q); q += 4) append_utf8(s, mem::r32(q));
            } else s = str(a);
            // honour width/precision through the host formatter
            int n = std::snprintf(nullptr, 0, (spec + "s").c_str(), s.c_str());
            std::string buf(n + 1, '\0');
            std::snprintf(buf.data(), n + 1, (spec + "s").c_str(), s.c_str());
            buf.resize(n);
            out += buf;
            break;
        }
        case 'p': {
            u32 a = va.next32();
            std::snprintf(tmp, sizeof tmp, "0x%x", a);
            out += tmp;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d = va.next_double();
            std::snprintf(tmp, sizeof tmp, (spec + conv).c_str(), d);
            out += tmp;
            break;
        }
        case 'n': {
            u32 a = va.next32();
            if (a) mem::w32(a, (u32)out.size());
            break;
        }
        case 'm': out += std::strerror(errno); break;
        default:
            out += '%';
            out += conv;
            break;
        }
    }
    return out;
}

int scan(const char* in, const char* fmt, abi::VarArgs& va) {
    const char* s = in;
    int assigned = 0;
    bool any_conv = false;
    for (const char* p = fmt; *p;) {
        if (std::isspace((unsigned char)*p)) {
            while (std::isspace((unsigned char)*p)) p++;
            while (std::isspace((unsigned char)*s)) s++;
            continue;
        }
        if (*p != '%') {
            if (*s != *p) return assigned ? assigned : (*s ? 0 : -1);
            s++;
            p++;
            continue;
        }
        p++;
        if (*p == '%') {
            while (std::isspace((unsigned char)*s)) s++;
            if (*s != '%') return assigned;
            s++;
            p++;
            continue;
        }
        bool suppress = false;
        if (*p == '*') { suppress = true; p++; }
        int width = 0;
        while (std::isdigit((unsigned char)*p)) width = width * 10 + (*p++ - '0');
        enum { NONE, HH, H, L, LL, BIGL, Z } len = NONE;
        if (p[0] == 'h' && p[1] == 'h') { len = HH; p += 2; }
        else if (*p == 'h') { len = H; p++; }
        else if (p[0] == 'l' && p[1] == 'l') { len = LL; p += 2; }
        else if (*p == 'l') { len = L; p++; }
        else if (*p == 'q' || *p == 'j') { len = LL; p++; }
        else if (*p == 'L') { len = BIGL; p++; }
        else if (*p == 'z' || *p == 't') { len = Z; p++; }
        char conv = *p++;
        std::string wspec = width ? std::to_string(width) : "";
        int consumed = 0;
        if (conv == 'n') {
            if (!suppress) mem::w32(va.next32(), (u32)(s - in));
            continue;
        }
        if (conv != 'c' && conv != '[') {
            while (std::isspace((unsigned char)*s)) s++;
        }
        if (!*s) return any_conv || assigned ? assigned : -1;
        any_conv = true;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            bool is_signed = conv == 'd' || conv == 'i';
            long long sv = 0;
            unsigned long long uv = 0;
            int r = is_signed ? std::sscanf(s, ("%" + wspec + "ll" + conv + "%n").c_str(), &sv, &consumed)
                              : std::sscanf(s, ("%" + wspec + "ll" + conv + "%n").c_str(), &uv, &consumed);
            if (r != 1) return assigned;
            s += consumed;
            if (suppress) break;
            u64 v = is_signed ? (u64)sv : uv;
            u32 dst = va.next32();
            switch (len) {
            case HH: mem::w8(dst, (u8)v); break;
            case H: mem::w16(dst, (u16)v); break;
            case LL: mem::w64(dst, v); break;
            default: mem::w32(dst, (u32)v); break;
            }
            assigned++;
            break;
        }
        case 'f': case 'e': case 'g': case 'E': case 'G': case 'a': {
            double d;
            if (std::sscanf(s, ("%" + wspec + "lf%n").c_str(), &d, &consumed) != 1) return assigned;
            s += consumed;
            if (suppress) break;
            u32 dst = va.next32();
            if (len == L || len == BIGL) mem::w64(dst, bit_cast_<u64>(d));
            else mem::w32(dst, bit_cast_<u32>((float)d));
            assigned++;
            break;
        }
        case 's': {
            std::string buf(std::strlen(s) + 1, '\0');
            if (std::sscanf(s, ("%" + wspec + "s%n").c_str(), buf.data(), &consumed) != 1) return assigned;
            s += consumed;
            if (suppress) break;
            u32 dst = va.next32();
            std::memcpy(mem::host(dst), buf.c_str(), std::strlen(buf.c_str()) + 1);
            assigned++;
            break;
        }
        case 'c': {
            int n = width ? width : 1;
            if ((int)std::strlen(s) < n) return assigned;
            if (!suppress) {
                std::memcpy(mem::host(va.next32()), s, n);
                assigned++;
            }
            s += n;
            break;
        }
        case '[': {
            std::string set = "[";
            if (*p == '^') set += *p++;
            if (*p == ']') set += *p++;
            while (*p && *p != ']') set += *p++;
            if (*p == ']') set += *p++;
            std::string buf(std::strlen(s) + 1, '\0');
            if (std::sscanf(s, ("%" + wspec + set + "%n").c_str(), buf.data(), &consumed) != 1) return assigned;
            s += consumed;
            if (!suppress) {
                std::memcpy(mem::host(va.next32()), buf.c_str(), std::strlen(buf.c_str()) + 1);
                assigned++;
            }
            break;
        }
        default: return assigned;
        }
    }
    return assigned;
}

namespace {

// ---- FILE* emulation --------------------------------------------------------
constexpr u32 SIZEOF_FILE = 84;  // bionic LP32 struct __sFILE
u32 g_sF = 0;
std::mutex g_files_mutex;
std::unordered_map<u32, FILE*> g_files;

FILE* hf(u32 gf) {
    if (gf >= g_sF && gf < g_sF + 3 * SIZEOF_FILE) {
        switch ((gf - g_sF) / SIZEOF_FILE) {
        case 0: return stdin;
        case 1: return stdout;
        default: return stderr;
        }
    }
    std::lock_guard lk(g_files_mutex);
    auto it = g_files.find(gf);
    return it == g_files.end() ? nullptr : it->second;
}
u32 wrap_file(FILE* f) {
    if (!f) return 0;
    u32 g = mem::heap_calloc(1, SIZEOF_FILE);
    mem::w16(g + 12, 0);  // _flags
    mem::w16(g + 14, (u16)fileno(f));  // _file
    std::lock_guard lk(g_files_mutex);
    g_files[g] = f;
    return g;
}

int write_out(FILE* f, const std::string& s) {
    if (f == stdout || f == stderr) {
        std::string line = s;
        while (!line.empty() && line.back() == '\n') line.pop_back();
        if (!line.empty()) logging::write(logging::Info, f == stdout ? "stdout" : "stderr", "%s", line.c_str());
        return (int)s.size();
    }
    return (int)std::fwrite(s.data(), 1, s.size(), f);
}

// Read-only opens of the game's data file go through the in-memory block
// cache (hle/file_cache.h): the game reads it in thousands of small pieces.
bool use_cache(const std::string& host_path, const char* mode) {
    if (std::strpbrk(mode, "wa+") || std::getenv("NFSMW_NO_OBB_CACHE")) return false;
    return host_path.size() > 4 && host_path.compare(host_path.size() - 4, 4, ".obb") == 0;
}

u32 h_fopen(u32 path, u32 mode) {
    std::string hp = map_path(str(path));
    FILE* f = use_cache(hp, str(mode)) ? hle::filecache::open(hp) : nullptr;
    if (!f) f = std::fopen(hp.c_str(), str(mode));
    if (!f) {
        set_errno_from_host();
        LOGD("fopen(%s, %s) failed: %s", str(path), str(mode), std::strerror(errno));
        return 0;
    }
    LOGD("fopen(%s, %s) -> %s", str(path), str(mode), hp.c_str());
    return wrap_file(f);
}
u32 h_fdopen(int fd, u32 mode) {
    FILE* f = ::fdopen(fd, str(mode));
    if (!f) set_errno_from_host();
    return wrap_file(f);
}
int h_fclose(u32 gf) {
    FILE* f = hf(gf);
    if (!f) return -1;
    if (f == stdin || f == stdout || f == stderr) return 0;
    {
        std::lock_guard lk(g_files_mutex);
        g_files.erase(gf);
    }
    int r = std::fclose(f);
    mem::heap_free(gf);
    return r;
}
u32 h_fread(u32 p, u32 size, u32 n, u32 gf) {
    FILE* f = hf(gf);
    if (!f) return 0;
    return (u32)std::fread(mem::host(p), size, n, f);
}
u32 h_fwrite(u32 p, u32 size, u32 n, u32 gf) {
    FILE* f = hf(gf);
    if (!f) return 0;
    if (f == stdout || f == stderr) {
        write_out(f, std::string((const char*)mem::host(p), (size_t)size * n));
        return n;
    }
    return (u32)std::fwrite(mem::host(p), size, n, f);
}
int h_fseek(u32 gf, s32 off, int whence) {
    FILE* f = hf(gf);
    return f ? std::fseek(f, off, whence) : -1;
}
s32 h_ftell(u32 gf) {
    FILE* f = hf(gf);
    return f ? (s32)std::ftell(f) : -1;
}
int h_feof(u32 gf) { FILE* f = hf(gf); return f ? std::feof(f) : 1; }
int h_ferror(u32 gf) { FILE* f = hf(gf); return f ? std::ferror(f) : 1; }
int h_fflush(u32 gf) {
    if (!gf) return std::fflush(nullptr);
    FILE* f = hf(gf);
    return f ? std::fflush(f) : -1;
}
int h_fgetc(u32 gf) { FILE* f = hf(gf); return f ? std::fgetc(f) : -1; }
int h_ungetc(int c, u32 gf) { FILE* f = hf(gf); return f ? std::ungetc(c, f) : -1; }
u32 h_fgets(u32 buf, int n, u32 gf) {
    FILE* f = hf(gf);
    if (!f) return 0;
    return std::fgets(ptr<char>(buf), n, f) ? buf : 0;
}
int h_fputc(int c, u32 gf) {
    FILE* f = hf(gf);
    if (!f) return -1;
    if (f == stdout || f == stderr) return c;
    return std::fputc(c, f);
}
int h_fileno(u32 gf) { FILE* f = hf(gf); return f ? ::fileno(f) : -1; }
void h_setbuf(u32, u32) {}
int h_fwide(u32, int mode) { return mode; }
#if defined(__SWITCH__)
int h_fsync(int) { return 0; }
#else
int h_fsync(int fd) { return ::fsync(fd); }
#endif

int h_fprintf(u32 gf, u32 fmt, abi::VarArgs va) {
    FILE* f = hf(gf);
    std::string s = format(str(fmt), va);
    return f ? write_out(f, s) : -1;
}
int h_vfprintf(u32 gf, u32 fmt, u32 ap) {
    auto va = abi::VarArgs::from_valist(ap);
    FILE* f = hf(gf);
    std::string s = format(str(fmt), va);
    return f ? write_out(f, s) : -1;
}
int h_printf(u32 fmt, abi::VarArgs va) { return write_out(stdout, format(str(fmt), va)); }
int h_puts(u32 s) { return write_out(stdout, std::string(str(s)) + "\n"); }
int h_putchar(int c) { return c; }
void h_perror(u32 s) { LOGW("perror: %s: %s", s ? str(s) : "", std::strerror((int)mem::r32(cpu::current().errno_addr()))); }

int h_sprintf(u32 buf, u32 fmt, abi::VarArgs va) {
    std::string s = format(str(fmt), va);
    std::memcpy(mem::host(buf), s.c_str(), s.size() + 1);
    return (int)s.size();
}
int put_n(u32 buf, u32 n, const std::string& s) {
    if (n) {
        u32 c = (u32)s.size() < n - 1 ? (u32)s.size() : n - 1;
        std::memcpy(mem::host(buf), s.data(), c);
        mem::w8(buf + c, 0);
    }
    return (int)s.size();
}
int h_snprintf(u32 buf, u32 n, u32 fmt, abi::VarArgs va) { return put_n(buf, n, format(str(fmt), va)); }
int h_vsnprintf(u32 buf, u32 n, u32 fmt, u32 ap) {
    auto va = abi::VarArgs::from_valist(ap);
    return put_n(buf, n, format(str(fmt), va));
}
int h_vasprintf(u32 out, u32 fmt, u32 ap) {
    auto va = abi::VarArgs::from_valist(ap);
    std::string s = format(str(fmt), va);
    mem::w32(out, mem::heap_strdup(s.c_str()));
    return (int)s.size();
}
int h_sscanf(u32 in, u32 fmt, abi::VarArgs va) { return scan(str(in), str(fmt), va); }
int h_vsscanf(u32 in, u32 fmt, u32 ap) {
    auto va = abi::VarArgs::from_valist(ap);
    return scan(str(in), str(fmt), va);
}

}  // namespace

void register_libc_stdio() {
    g_sF = mem::static_alloc(3 * SIZEOF_FILE, 8);
    for (int i = 0; i < 3; i++) mem::w16(g_sF + i * SIZEOF_FILE + 14, (u16)i);
    thunks::add_data("__sF", g_sF);
    REG("fopen", h_fopen);
    REG("fdopen", h_fdopen);
    REG("fclose", h_fclose);
    REG("fread", h_fread);
    REG("fwrite", h_fwrite);
    REG("fseek", h_fseek);
    REG("fseeko", h_fseek);
    REG("ftell", h_ftell);
    REG("ftello", h_ftell);
    REG("feof", h_feof);
    REG("ferror", h_ferror);
    REG("fflush", h_fflush);
    REG("fgetc", h_fgetc);
    REG("getc", h_fgetc);
    REG("ungetc", h_ungetc);
    REG("fgets", h_fgets);
    REG("fputc", h_fputc);
    REG("putc", h_fputc);
    REG("fileno", h_fileno);
    REG("setbuf", h_setbuf);
    REG("fwide", h_fwide);
    REG("fsync", h_fsync);
    REG("fprintf", h_fprintf);
    REG("vfprintf", h_vfprintf);
    REG("printf", h_printf);
    REG("puts", h_puts);
    REG("putchar", h_putchar);
    REG("perror", h_perror);
    REG("sprintf", h_sprintf);
    REG("snprintf", h_snprintf);
    REG("vsnprintf", h_vsnprintf);
    REG("vasprintf", h_vasprintf);
    REG("sscanf", h_sscanf);
    REG("vsscanf", h_vsscanf);
}

}  // namespace hle
