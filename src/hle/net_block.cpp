// Online services are blocked.
//
// The game talks to EA's servers (Blaze/Autolog, Synergy, Nimble telemetry,
// update checks) only through the BSD socket API and the resolver. Every one
// of those entry points is refused here, so no connection can ever be made:
// the game sees a device without network and runs fully offline. The Switch
// build does not even initialise the console's socket service.
#include <atomic>
#include <mutex>
#include <set>

#include "hle/hle.h"

namespace hle {

namespace {
constexpr int kEACCES = 13;
constexpr int kEAI_FAIL = 4;  // bionic numbering (positive)

std::atomic<u32> g_attempts{0};
std::mutex g_seen_mutex;
std::set<std::string> g_seen;

void note(const char* fn) {
    g_attempts.fetch_add(1);
    std::lock_guard lk(g_seen_mutex);
    if (g_seen.insert(fn).second) {
        u32 lr = cpu::current_or_null() ? cpu::current().regs()[14] : 0;
        LOGW("online services blocked: %s() refused (called from 0x%08X)", fn, lr);
    }
}

thunks::Fn refuse(const char* fn) {
    return [fn](cpu::GuestThread& t) {
        note(fn);
        t.set_errno(kEACCES);
        t.regs()[0] = (u32)-1;
    };
}

int h_getaddrinfo(u32 node, u32, u32, u32 res) {
    note("getaddrinfo");
    if (node) LOGD("blocked DNS lookup: %s", mem::str(node));
    if (res) mem::w32(res, 0);
    return kEAI_FAIL;
}
int h_getnameinfo(u32, u32, u32, u32, u32) {
    note("getnameinfo");
    return kEAI_FAIL;
}
u32 h_gethostbyname(u32 name) {
    note("gethostbyname");
    if (name) LOGD("blocked DNS lookup: %s", mem::str(name));
    return 0;
}
void h_freeaddrinfo(u32) {}
u32 h_gai_strerror(int) { return intern_string("network access disabled"); }
u32 h_if_nametoindex(u32) { return 0; }

// Pure address conversions (no network access).
u32 h_inet_addr(u32 s) {
    unsigned a, b, c, d;
    if (std::sscanf(mem::str(s), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0xFFFFFFFF;
    return a | (b << 8) | (c << 16) | (d << 24);
}
int h_inet_pton(int af, u32 src, u32 dst) {
    if (af != 2) return 0;
    u32 v = h_inet_addr(src);
    if (v == 0xFFFFFFFF) return 0;
    mem::w32(dst, v);
    return 1;
}
u32 h_inet_ntop(int af, u32 src, u32 dst, u32 size) {
    if (af != 2) return 0;
    u32 v = mem::r32(src);
    std::snprintf(mem::ptr<char>(dst), size, "%u.%u.%u.%u", v & 255, (v >> 8) & 255, (v >> 16) & 255, v >> 24);
    return dst;
}
}  // namespace

u32 blocked_network_attempts() { return g_attempts.load(); }

void register_net_block() {
    for (const char* n : {"socket", "socketpair", "bind", "connect", "listen", "accept", "send", "sendto", "recv",
                          "recvfrom", "sendmsg", "recvmsg", "shutdown", "setsockopt", "getsockopt", "getpeername",
                          "getsockname"})
        REG_RAW(n, refuse(n));
    REG("getaddrinfo", h_getaddrinfo);
    REG("getnameinfo", h_getnameinfo);
    REG("gethostbyname", h_gethostbyname);
    REG("gethostbyname2", h_gethostbyname);
    REG("gethostbyaddr", h_gethostbyname);
    REG("freeaddrinfo", h_freeaddrinfo);
    REG("gai_strerror", h_gai_strerror);
    REG("if_nametoindex", h_if_nametoindex);
    REG("inet_addr", h_inet_addr);
    REG("inet_pton", h_inet_pton);
    REG("inet_ntop", h_inet_ntop);
    LOGI("online services: blocked (no sockets, no DNS)");
}

}  // namespace hle
