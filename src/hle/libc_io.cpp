// File system and descriptor I/O. Android paths are mapped into the port's
// folder (game/paths.h): the game's own storage goes to save/, the OBB to game/.
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>

#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "game/config.h"
#include "game/paths.h"
#include "hle/hle.h"
#include "platform/platform.h"

namespace hle {

namespace {
using mem::ptr;
using mem::str;

std::mutex g_cwd_mutex;
std::string g_cwd = "/";

std::string normalize(const std::string& p) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string part = p.substr(i, j - i);
        if (part.empty() || part == ".") {
        } else if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(part);
        }
        i = j + 1;
    }
    std::string out;
    for (auto& s : parts) out += "/" + s;
    return out.empty() ? "/" : out;
}
}  // namespace

std::string map_path(const char* gp) {
    if (!gp) return "";
    std::string p = gp;
    if (p.empty()) return "";
    if (p[0] != '/') {
        std::lock_guard lk(g_cwd_mutex);
        p = g_cwd + "/" + p;
    }
    return game::paths::from_android(normalize(p));
}

#if defined(__SWITCH__)
// newlib numbers its errno values differently from Linux above ~35.
int guest_errno_from_host(int e) {
    switch (e) {
    case ENOSYS: return 38;
    case ENOTEMPTY: return 39;
    case ENAMETOOLONG: return 36;
    case ELOOP: return 40;
    case ETIMEDOUT: return 110;
    case ENOTSUP: return 95;
    case ECONNREFUSED: return 111;
    case ENETUNREACH: return 101;
    default: return e < 35 ? e : 5 /* EIO */;
    }
}
#else
int guest_errno_from_host(int e) { return e; }  // Linux host: same numbering as Android
#endif
void set_errno_from_host() {
    if (auto* t = cpu::current_or_null()) t->set_errno(guest_errno_from_host(errno));
}

namespace {

// Android/ARM open() flags differ from x86-64 Linux for a few bits.
int host_open_flags(u32 g) {
    int f = g & 3;  // access mode
    if (g & 0100) f |= O_CREAT;
    if (g & 0200) f |= O_EXCL;
#ifdef O_NOCTTY
    if (g & 0400) f |= O_NOCTTY;
#endif
    if (g & 01000) f |= O_TRUNC;
    if (g & 02000) f |= O_APPEND;
    if (g & 04000) f |= O_NONBLOCK;
#ifdef O_DIRECTORY
    if (g & 040000) f |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    if (g & 0100000) f |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    if (g & 02000000) f |= O_CLOEXEC;
#endif
    return f;
}

int h_open(u32 path, u32 flags, abi::VarArgs va) {
    u32 mode = (flags & 0100) ? va.next32() : 0;
    std::string hp = map_path(str(path));
    int fd = ::open(hp.c_str(), host_open_flags(flags), mode);
    if (fd < 0) {
        set_errno_from_host();
        LOGD("open(%s) failed: %s", str(path), std::strerror(errno));
    } else {
        LOGD("open(%s) -> fd %d", str(path), fd);
    }
    return fd;
}
int h_close(int fd) {
    if (fd <= 2) return 0;
    int r = ::close(fd);
    if (r < 0) set_errno_from_host();
    return r;
}
s32 h_read(int fd, u32 buf, u32 n) {
    ssize_t r = ::read(fd, mem::host(buf), n);
    if (r < 0) set_errno_from_host();
    return (s32)r;
}
s32 h_write(int fd, u32 buf, u32 n) {
    if (fd == 1 || fd == 2) {
        std::string s((const char*)mem::host(buf), n);
        while (!s.empty() && s.back() == '\n') s.pop_back();
        LOGI("fd%d: %s", fd, s.c_str());
        return (s32)n;
    }
    ssize_t r = ::write(fd, mem::host(buf), n);
    if (r < 0) set_errno_from_host();
    return (s32)r;
}
s32 h_lseek(int fd, s32 off, int whence) {
    off_t r = ::lseek(fd, off, whence);
    if (r < 0) set_errno_from_host();
    return (s32)r;
}
s64 h_lseek64(int fd, s64 off, int whence) {
    off_t r = ::lseek(fd, off, whence);
    if (r < 0) set_errno_from_host();
    return r;
}
int h_ftruncate(int fd, s32 len) { return ::ftruncate(fd, len); }
#if defined(__SWITCH__)
int h_dup2(int, int) { return -1; }
int h_fcntl(int, int, abi::VarArgs) { return 0; }
#else
int h_dup2(int a, int b) { return ::dup2(a, b); }
int h_fcntl(int fd, int cmd, abi::VarArgs va) {
    u32 arg = va.next32();
    switch (cmd) {
    case 1: return ::fcntl(fd, F_GETFD);
    case 2: return ::fcntl(fd, F_SETFD, arg);
    case 3: return ::fcntl(fd, F_GETFL) & 3;
    case 4: return 0;
    default: LOGD("fcntl(%d, %d) ignored", fd, cmd); return 0;
    }
}
#endif
int h_ioctl(int, u32, abi::VarArgs) {
    cpu::current().set_errno(25);  // ENOTTY
    return -1;
}

// bionic LP32 struct stat (== kernel struct stat64 on ARM), 104 bytes
void put_stat(u32 g, const struct stat& st) {
    std::memset(mem::host(g), 0, 104);
    mem::w64(g + 0, st.st_dev);
    mem::w32(g + 12, (u32)st.st_ino);
    mem::w32(g + 16, st.st_mode);
    mem::w32(g + 20, (u32)st.st_nlink);
    mem::w32(g + 24, st.st_uid);
    mem::w32(g + 28, st.st_gid);
    mem::w64(g + 32, st.st_rdev);
    mem::w64(g + 48, (u64)st.st_size);
    mem::w32(g + 56, (u32)st.st_blksize);
    mem::w64(g + 64, (u64)st.st_blocks);
    mem::w32(g + 72, (u32)st.st_atime);
    mem::w32(g + 80, (u32)st.st_mtime);
    mem::w32(g + 88, (u32)st.st_ctime);
    mem::w64(g + 96, st.st_ino);
}
int h_stat(u32 path, u32 buf) {
    struct stat st;
    std::string hp = map_path(str(path));
    if (::stat(hp.c_str(), &st) < 0) {
        set_errno_from_host();
        LOGD("stat(%s) failed", str(path));
        return -1;
    }
    put_stat(buf, st);
    return 0;
}
int h_fstat(int fd, u32 buf) {
    struct stat st;
    if (::fstat(fd, &st) < 0) {
        set_errno_from_host();
        return -1;
    }
    put_stat(buf, st);
    return 0;
}
int h_statfs(u32 path, u32 buf) {
    (void)path;
    std::memset(mem::host(buf), 0, 88);
    mem::w32(buf + 0, 0xEF53);
    mem::w32(buf + 4, 4096);
    mem::w64(buf + 8, 4u * 1024 * 1024);   // 16 GiB of blocks
    mem::w64(buf + 16, 2u * 1024 * 1024);  // 8 GiB free
    mem::w64(buf + 24, 2u * 1024 * 1024);
    mem::w64(buf + 32, 1000000);
    mem::w64(buf + 40, 500000);
    mem::w32(buf + 56, 255);
    mem::w32(buf + 60, 4096);
    return 0;
}
int h_access(u32 path, int mode) {
    std::string hp = map_path(str(path));
#if defined(__SWITCH__)
    (void)mode;
    struct stat st;
    int r = ::stat(hp.c_str(), &st);
#else
    int r = ::access(hp.c_str(), mode);
#endif
    if (r < 0) set_errno_from_host();
    LOGD("access(%s) -> %d", str(path), r);
    return r;
}
// Create intermediate directories too: Android apps expect their data dirs to exist.
int mkdirs(const std::string& hp, mode_t mode) {
    for (size_t i = 1; i < hp.size(); i++)
        if (hp[i] == '/') ::mkdir(hp.substr(0, i).c_str(), 0755);
    return ::mkdir(hp.c_str(), mode);
}
int h_mkdir(u32 path, u32 mode) {
    int r = mkdirs(map_path(str(path)), mode);
    if (r < 0) set_errno_from_host();
    return r;
}
int h_rmdir(u32 path) { int r = ::rmdir(map_path(str(path)).c_str()); if (r < 0) set_errno_from_host(); return r; }
int h_unlink(u32 path) { int r = ::unlink(map_path(str(path)).c_str()); if (r < 0) set_errno_from_host(); return r; }
int h_rename(u32 a, u32 b) {
    int r = ::rename(map_path(str(a)).c_str(), map_path(str(b)).c_str());
    if (r < 0) set_errno_from_host();
    return r;
}
int h_chmod(u32, u32) { return 0; }
int h_chown(u32, u32, u32) { return 0; }
int h_utime(u32, u32) { return 0; }
s32 h_readlink(u32, u32, u32) {
    cpu::current().set_errno(22);
    return -1;
}
int h_chdir(u32 path) {
    std::lock_guard lk(g_cwd_mutex);
    std::string p = str(path);
    g_cwd = normalize(p[0] == '/' ? p : g_cwd + "/" + p);
    return 0;
}
u32 h_getcwd(u32 buf, u32 size) {
    std::lock_guard lk(g_cwd_mutex);
    if (!buf) return mem::heap_strdup(g_cwd.c_str());
    if (g_cwd.size() + 1 > size) return 0;
    std::memcpy(mem::host(buf), g_cwd.c_str(), g_cwd.size() + 1);
    return buf;
}

// ---- directories (DIR* is a guest block holding one struct dirent) ----------
constexpr u32 SIZEOF_DIRENT = 280;
std::mutex g_dir_mutex;
std::unordered_map<u32, DIR*> g_dirs;

u32 h_opendir(u32 path) {
    DIR* d = ::opendir(map_path(str(path)).c_str());
    if (!d) {
        set_errno_from_host();
        return 0;
    }
    u32 g = mem::heap_calloc(1, SIZEOF_DIRENT + 16);
    std::lock_guard lk(g_dir_mutex);
    g_dirs[g] = d;
    return g;
}
DIR* hdir(u32 g) {
    std::lock_guard lk(g_dir_mutex);
    auto it = g_dirs.find(g);
    return it == g_dirs.end() ? nullptr : it->second;
}
void fill_dirent(u32 e, const struct dirent* de) {
    std::memset(mem::host(e), 0, SIZEOF_DIRENT);
    mem::w64(e + 0, de->d_ino);
#if !defined(__SWITCH__)
    mem::w64(e + 8, (u64)de->d_off);
#endif
    mem::w16(e + 16, SIZEOF_DIRENT);
    mem::w8(e + 18, de->d_type);
    std::strncpy(ptr<char>(e + 19), de->d_name, 255);
}
u32 h_readdir(u32 g) {
    DIR* d = hdir(g);
    if (!d) return 0;
    struct dirent* de = ::readdir(d);
    if (!de) return 0;
    u32 e = g + 16;
    fill_dirent(e, de);
    return e;
}
int h_readdir_r(u32 g, u32 entry, u32 result) {
    DIR* d = hdir(g);
    struct dirent* de = d ? ::readdir(d) : nullptr;
    if (!de) {
        mem::w32(result, 0);
        return 0;
    }
    fill_dirent(entry, de);
    mem::w32(result, entry);
    return 0;
}
int h_closedir(u32 g) {
    DIR* d;
    {
        std::lock_guard lk(g_dir_mutex);
        auto it = g_dirs.find(g);
        if (it == g_dirs.end()) return -1;
        d = it->second;
        g_dirs.erase(it);
    }
    ::closedir(d);
    mem::heap_free(g);
    return 0;
}

// ---- memory mapping ---------------------------------------------------------
std::mutex g_mmap_mutex;
std::map<u32, u32> g_mmaps;

u32 h_mmap(u32 addr, u32 len, int prot, int flags, int fd, s32 off) {
    (void)addr;
    (void)prot;
    u32 p = mem::heap_alloc(len, 4096);
    if (!p) {
        cpu::current().set_errno(12);
        return 0xFFFFFFFF;
    }
    std::memset(mem::host(p), 0, len);
    if (!(flags & 0x20) && fd >= 0) {  // not MAP_ANONYMOUS: copy the file contents
#if defined(__SWITCH__)
        off_t saved = ::lseek(fd, 0, SEEK_CUR);
        ::lseek(fd, off, SEEK_SET);
        ssize_t r = ::read(fd, mem::host(p), len);
        ::lseek(fd, saved, SEEK_SET);
#else
        ssize_t r = ::pread(fd, mem::host(p), len, off);
#endif
        LOGD("mmap(fd=%d, off=%d, len=%u) copied %zd bytes", fd, off, len, r);
    }
    std::lock_guard lk(g_mmap_mutex);
    g_mmaps[p] = len;
    return p;
}
int h_munmap(u32 addr, u32 len) {
    (void)len;
    std::lock_guard lk(g_mmap_mutex);
    auto it = g_mmaps.find(addr);
    if (it != g_mmaps.end()) {
        g_mmaps.erase(it);
        mem::heap_free(addr);
    }
    return 0;
}
int h_mprotect(u32, u32, int) { return 0; }
int h_mlock(u32, u32) { return 0; }
u32 h_sbrk(s32) {
    cpu::current().set_errno(12);
    return 0xFFFFFFFF;
}

// ---- polling (no real network: just honour the timeout) ---------------------
int h_poll(u32 fds, u32 n, int timeout) {
    (void)fds;
    (void)n;
    if (timeout > 0) std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
    return 0;
}
int h_select(int, u32, u32, u32, u32 tv) {
    if (tv) {
        s32 sec = (s32)mem::r32(tv), usec = (s32)mem::r32(tv + 4);
        std::this_thread::sleep_for(std::chrono::microseconds((s64)sec * 1000000 + usec));
    }
    return 0;
}

}  // namespace

void register_libc_io() {
    REG("open", h_open);
    REG("close", h_close);
    REG("read", h_read);
    REG("write", h_write);
    REG("lseek", h_lseek);
    REG("lseek64", h_lseek64);
    REG("ftruncate", h_ftruncate);
    REG("dup2", h_dup2);
    REG("fcntl", h_fcntl);
    REG("ioctl", h_ioctl);
    REG("stat", h_stat);
    REG("fstat", h_fstat);
    REG("statfs", h_statfs);
    REG("access", h_access);
    REG("mkdir", h_mkdir);
    REG("rmdir", h_rmdir);
    REG("unlink", h_unlink);
    REG("rename", h_rename);
    REG("chmod", h_chmod);
    REG("chown", h_chown);
    REG("utime", h_utime);
    REG("readlink", h_readlink);
    REG("chdir", h_chdir);
    REG("getcwd", h_getcwd);
    REG("opendir", h_opendir);
    REG("readdir", h_readdir);
    REG("readdir_r", h_readdir_r);
    REG("closedir", h_closedir);
    REG("mmap", h_mmap);
    REG("munmap", h_munmap);
    REG("mprotect", h_mprotect);
    REG("mlock", h_mlock);
    REG("sbrk", h_sbrk);
    REG("poll", h_poll);
    REG("select", h_select);
}

}  // namespace hle
