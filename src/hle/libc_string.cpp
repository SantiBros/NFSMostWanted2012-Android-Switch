// malloc family, memory/string functions, ctype and wide-char helpers.
#include <strings.h>

#include <cctype>
#include <climits>
#include <cwctype>
#include <mutex>
#include <unordered_map>

#include "hle/hle.h"

namespace hle {

namespace {

using mem::ptr;
using mem::str;

// ---- allocation -----------------------------------------------------------
u32 h_malloc(u32 n) { return mem::heap_alloc(n); }
void h_free(u32 p) { mem::heap_free(p); }
u32 h_calloc(u32 n, u32 s) { return mem::heap_calloc(n, s); }
u32 h_realloc(u32 p, u32 n) { return mem::heap_realloc(p, n); }
u32 h_memalign(u32 align, u32 n) { return mem::heap_alloc(n, align < 16 ? 16 : align); }

// ---- memory ---------------------------------------------------------------
u32 h_memcpy(u32 d, u32 s, u32 n) { std::memmove(mem::host(d), mem::host(s), n); return d; }
u32 h_memmove(u32 d, u32 s, u32 n) { std::memmove(mem::host(d), mem::host(s), n); return d; }
u32 h_memset(u32 d, int c, u32 n) { std::memset(mem::host(d), c, n); return d; }
int h_memcmp(u32 a, u32 b, u32 n) { return std::memcmp(mem::host(a), mem::host(b), n); }
u32 h_memchr(u32 s, int c, u32 n) {
    auto* p = (const u8*)std::memchr(mem::host(s), c, n);
    return p ? mem::guest(p) : 0;
}
u32 h_memrchr(u32 s, int c, u32 n) {
    const u8* b = mem::host(s);
    for (u32 i = n; i-- > 0;)
        if (b[i] == (u8)c) return s + i;
    return 0;
}
u32 h_memmem(u32 h, u32 hl, u32 n, u32 nl) {
    if (nl == 0) return h;
    const u8* hp = mem::host(h);
    const u8* np = mem::host(n);
    for (u32 i = 0; i + nl <= hl; i++)
        if (hp[i] == np[0] && std::memcmp(hp + i, np, nl) == 0) return h + i;
    return 0;
}
// ARM run-time ABI variants (note the argument order of memset/memclr).
void h_aeabi_memcpy(u32 d, u32 s, u32 n) { std::memcpy(mem::host(d), mem::host(s), n); }
void h_aeabi_memmove(u32 d, u32 s, u32 n) { std::memmove(mem::host(d), mem::host(s), n); }
void h_aeabi_memset(u32 d, u32 n, int c) { std::memset(mem::host(d), c, n); }
void h_aeabi_memclr(u32 d, u32 n) { std::memset(mem::host(d), 0, n); }

// ---- strings --------------------------------------------------------------
u32 h_strlen(u32 s) { return (u32)std::strlen(str(s)); }
int h_strcmp(u32 a, u32 b) { return std::strcmp(str(a), str(b)); }
int h_strncmp(u32 a, u32 b, u32 n) { return std::strncmp(str(a), str(b), n); }
int h_strcasecmp(u32 a, u32 b) { return ::strcasecmp(str(a), str(b)); }
int h_strncasecmp(u32 a, u32 b, u32 n) { return ::strncasecmp(str(a), str(b), n); }
int h_strcoll(u32 a, u32 b) { return std::strcmp(str(a), str(b)); }
u32 h_strxfrm(u32 d, u32 s, u32 n) {
    u32 len = (u32)std::strlen(str(s));
    if (n) {
        u32 c = len < n - 1 ? len : n - 1;
        std::memcpy(mem::host(d), str(s), c);
        mem::w8(d + c, 0);
    }
    return len;
}
u32 h_strcpy(u32 d, u32 s) { std::memmove(mem::host(d), str(s), std::strlen(str(s)) + 1); return d; }
u32 h_strncpy(u32 d, u32 s, u32 n) { std::strncpy(ptr<char>(d), str(s), n); return d; }
u32 h_strcat(u32 d, u32 s) { std::strcat(ptr<char>(d), str(s)); return d; }
u32 h_strchr(u32 s, int c) {
    const char* p = std::strchr(str(s), c);
    return p ? mem::guest(p) : 0;
}
u32 h_strrchr(u32 s, int c) {
    const char* p = std::strrchr(str(s), c);
    return p ? mem::guest(p) : 0;
}
u32 h_strstr(u32 h, u32 n) {
    const char* p = std::strstr(str(h), str(n));
    return p ? mem::guest(p) : 0;
}
u32 h_strpbrk(u32 s, u32 a) {
    const char* p = std::strpbrk(str(s), str(a));
    return p ? mem::guest(p) : 0;
}
u32 h_strspn(u32 s, u32 a) { return (u32)std::strspn(str(s), str(a)); }
u32 h_strcspn(u32 s, u32 a) { return (u32)std::strcspn(str(s), str(a)); }
u32 h_strdup(u32 s) { return s ? mem::heap_strdup(str(s)) : 0; }
// strtok_r over guest memory (the host one cannot be given a guest save pointer).
u32 g_strtok_r(u32 s, u32 delim, u32 saveptr) {
    u32 p = s ? s : mem::r32(saveptr);
    if (!p) return 0;
    const char* d = str(delim);
    while (mem::r8(p) && std::strchr(d, mem::r8(p))) p++;
    if (!mem::r8(p)) {
        mem::w32(saveptr, p);
        return 0;
    }
    u32 tok = p;
    while (mem::r8(p) && !std::strchr(d, mem::r8(p))) p++;
    if (mem::r8(p)) {
        mem::w8(p, 0);
        p++;
    }
    mem::w32(saveptr, p);
    return tok;
}

// interned, never freed: strerror() and friends hand out these
std::mutex g_intern_mutex;
std::unordered_map<std::string, u32> g_interned;
u32 intern(const std::string& s) {
    std::lock_guard lk(g_intern_mutex);
    auto it = g_interned.find(s);
    if (it != g_interned.end()) return it->second;
    u32 a = mem::static_strdup(s.c_str());
    g_interned.emplace(s, a);
    return a;
}

u32 h_strerror(int e) { return intern(std::strerror(e)); }
int h_strerror_r(int e, u32 buf, u32 n) {
    if (n) {
        std::snprintf(ptr<char>(buf), n, "%s", std::strerror(e));
    }
    return 0;
}
u32 h_basename(u32 path) {
    if (!path) return intern(".");
    std::string p = str(path);
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    auto slash = p.find_last_of('/');
    std::string b = slash == std::string::npos ? p : p.substr(slash + 1);
    if (b.empty()) b = "/";
    return intern(b);
}

// ---- number parsing --------------------------------------------------------
template <class F>
auto parse_with_end(u32 s, u32 endp, F f) {
    char* end = nullptr;
    auto v = f(str(s), &end);
    if (endp) mem::w32(endp, s + (u32)(end - str(s)));
    return v;
}
s32 clamp32(long long v) { return v > INT_MAX ? INT_MAX : v < INT_MIN ? INT_MIN : (s32)v; }
s32 h_strtol(u32 s, u32 e, int base) {
    errno = 0;
    long long v = parse_with_end(s, e, [base](const char* p, char** end) { return std::strtoll(p, end, base); });
    if (v > INT_MAX || v < INT_MIN) cpu::current().set_errno(ERANGE);
    return clamp32(v);
}
u32 h_strtoul(u32 s, u32 e, int base) {
    unsigned long long v =
        parse_with_end(s, e, [base](const char* p, char** end) { return std::strtoull(p, end, base); });
    // negative inputs wrap like a 32-bit unsigned long
    if (v > 0xFFFFFFFFull && (long long)v > 0) return 0xFFFFFFFFu;
    return (u32)v;
}
s64 h_strtoll(u32 s, u32 e, int base) {
    return parse_with_end(s, e, [base](const char* p, char** end) { return std::strtoll(p, end, base); });
}
u64 h_strtoull(u32 s, u32 e, int base) {
    return parse_with_end(s, e, [base](const char* p, char** end) { return std::strtoull(p, end, base); });
}
double h_strtod(u32 s, u32 e) {
    return parse_with_end(s, e, [](const char* p, char** end) { return std::strtod(p, end); });
}
int h_atoi(u32 s) { return std::atoi(str(s)); }

// ---- ctype -----------------------------------------------------------------
int h_isalnum(int c) { return c >= 0 && c < 256 && std::isalnum(c); }
int h_isalpha(int c) { return c >= 0 && c < 256 && std::isalpha(c); }
int h_isblank(int c) { return c == ' ' || c == '\t'; }
int h_isgraph(int c) { return c >= 0 && c < 256 && std::isgraph(c); }
int h_islower(int c) { return c >= 'a' && c <= 'z'; }
int h_isupper(int c) { return c >= 'A' && c <= 'Z'; }
int h_isprint(int c) { return c >= 0x20 && c < 0x7F; }
int h_isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int h_isxdigit(int c) { return c >= 0 && c < 256 && std::isxdigit(c); }
int h_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int h_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

int h_iswalpha(u32 c) { return std::iswalpha(c); }
int h_iswcntrl(u32 c) { return std::iswcntrl(c); }
int h_iswdigit(u32 c) { return c >= '0' && c <= '9'; }
int h_iswlower(u32 c) { return std::iswlower(c); }
int h_iswupper(u32 c) { return std::iswupper(c); }
int h_iswprint(u32 c) { return std::iswprint(c); }
int h_iswpunct(u32 c) { return std::iswpunct(c); }
int h_iswspace(u32 c) { return std::iswspace(c); }
int h_iswxdigit(u32 c) { return std::iswxdigit(c); }
u32 h_towlower(u32 c) { return std::towlower(c); }
u32 h_towupper(u32 c) { return std::towupper(c); }
u32 h_btowc(int c) { return (c >= 0 && c < 0x80) ? (u32)c : 0xFFFFFFFFu; }
int h_wctob(u32 c) { return c < 0x80 ? (int)c : -1; }
int h_wcscoll(u32 a, u32 b) {
    for (;; a += 4, b += 4) {
        u32 x = mem::r32(a), y = mem::r32(b);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}
u32 h_wcsxfrm(u32 d, u32 s, u32 n) {
    u32 len = 0;
    while (mem::r32(s + len * 4)) len++;
    if (n) {
        u32 c = len < n - 1 ? len : n - 1;
        std::memmove(mem::host(d), mem::host(s), c * 4);
        mem::w32(d + c * 4, 0);
    }
    return len;
}
int h_mbrlen(u32 s, u32 n, u32) {
    if (!s) return 0;
    if (n == 0) return -2;
    u8 c = mem::r8(s);
    if (c == 0) return 0;
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : -1;
    if (len < 0) return -1;
    return (u32)len <= n ? len : -2;
}

// OpenBSD-style ctype tables used by bionic's ctype macros:
// _ctype_ points at a 257-entry table indexed by c+1.
void setup_ctype_tables() {
    enum { U = 0x01, L = 0x02, N = 0x04, S = 0x08, P = 0x10, C = 0x20, X = 0x40, B = 0x80 };
    u32 table = mem::static_alloc(257, 4);
    for (int c = 0; c < 256; c++) {
        u8 v = 0;
        if (c < 128) {
            if (std::isupper(c)) v |= U;
            if (std::islower(c)) v |= L;
            if (std::isdigit(c)) v |= N;
            if (std::isspace(c)) v |= S;
            if (std::ispunct(c)) v |= P;
            if (std::iscntrl(c)) v |= C;
            if (std::isxdigit(c) && !std::isdigit(c)) v |= X;
            if (c == ' ') v |= B;
        }
        mem::w8(table + 1 + c, v);
    }
    u32 ctype_ptr = mem::static_alloc(4, 4);
    mem::w32(ctype_ptr, table);
    thunks::add_data("_ctype_", ctype_ptr);

    u32 lower = mem::static_alloc(257 * 2, 4);
    u32 upper = mem::static_alloc(257 * 2, 4);
    mem::w16(lower, 0xFFFF);
    mem::w16(upper, 0xFFFF);
    for (int c = 0; c < 256; c++) {
        mem::w16(lower + 2 + c * 2, (u16)h_tolower(c));
        mem::w16(upper + 2 + c * 2, (u16)h_toupper(c));
    }
    u32 lp = mem::static_alloc(4, 4), up = mem::static_alloc(4, 4);
    mem::w32(lp, lower);
    mem::w32(up, upper);
    thunks::add_data("_tolower_tab_", lp);
    thunks::add_data("_toupper_tab_", up);
}

}  // namespace

u32 intern_string(const std::string& s) { return intern(s); }

void register_libc_string() {
    REG("malloc", h_malloc);
    REG("free", h_free);
    REG("calloc", h_calloc);
    REG("realloc", h_realloc);
    REG("memalign", h_memalign);
    REG("memcpy", h_memcpy);
    REG("memmove", h_memmove);
    REG("memset", h_memset);
    REG("memcmp", h_memcmp);
    REG("memchr", h_memchr);
    REG("memrchr", h_memrchr);
    REG("memmem", h_memmem);
    for (const char* n : {"__aeabi_memcpy", "__aeabi_memcpy4", "__aeabi_memcpy8"}) REG(n, h_aeabi_memcpy);
    for (const char* n : {"__aeabi_memmove", "__aeabi_memmove4", "__aeabi_memmove8"}) REG(n, h_aeabi_memmove);
    for (const char* n : {"__aeabi_memset", "__aeabi_memset4", "__aeabi_memset8"}) REG(n, h_aeabi_memset);
    for (const char* n : {"__aeabi_memclr", "__aeabi_memclr4", "__aeabi_memclr8"}) REG(n, h_aeabi_memclr);
    REG("strlen", h_strlen);
    REG("strcmp", h_strcmp);
    REG("strncmp", h_strncmp);
    REG("strcasecmp", h_strcasecmp);
    REG("strncasecmp", h_strncasecmp);
    REG("strcoll", h_strcoll);
    REG("strxfrm", h_strxfrm);
    REG("strcpy", h_strcpy);
    REG("strncpy", h_strncpy);
    REG("strcat", h_strcat);
    REG("strchr", h_strchr);
    REG("strrchr", h_strrchr);
    REG("strstr", h_strstr);
    REG("strpbrk", h_strpbrk);
    REG("strspn", h_strspn);
    REG("strcspn", h_strcspn);
    REG("strdup", h_strdup);
    REG("strtok_r", g_strtok_r);
    REG("strerror", h_strerror);
    REG("strerror_r", h_strerror_r);
    REG("basename", h_basename);
    REG("strtol", h_strtol);
    REG("strtoul", h_strtoul);
    REG("strtoll", h_strtoll);
    REG("strtoull", h_strtoull);
    REG("strtoimax", h_strtoll);
    REG("strtoumax", h_strtoull);
    REG("strtod", h_strtod);
    REG("atoi", h_atoi);
    REG("isalnum", h_isalnum);
    REG("isalpha", h_isalpha);
    REG("isblank", h_isblank);
    REG("isgraph", h_isgraph);
    REG("islower", h_islower);
    REG("isupper", h_isupper);
    REG("isprint", h_isprint);
    REG("isspace", h_isspace);
    REG("isxdigit", h_isxdigit);
    REG("tolower", h_tolower);
    REG("toupper", h_toupper);
    REG("iswalpha", h_iswalpha);
    REG("iswcntrl", h_iswcntrl);
    REG("iswdigit", h_iswdigit);
    REG("iswlower", h_iswlower);
    REG("iswupper", h_iswupper);
    REG("iswprint", h_iswprint);
    REG("iswpunct", h_iswpunct);
    REG("iswspace", h_iswspace);
    REG("iswxdigit", h_iswxdigit);
    REG("towlower", h_towlower);
    REG("towupper", h_towupper);
    REG("btowc", h_btowc);
    REG("wctob", h_wctob);
    REG("wcscoll", h_wcscoll);
    REG("wcsxfrm", h_wcsxfrm);
    REG("mbrlen", h_mbrlen);
    setup_ctype_tables();
}

}  // namespace hle
