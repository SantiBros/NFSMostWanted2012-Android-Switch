#include "hle/file_cache.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "core/common.h"
#include "core/cpu.h"
#include "platform/thread.h"

namespace hle::filecache {

namespace {

constexpr uint64_t kBlock = 128 * 1024;
constexpr uint64_t kCapacity = 128ull * 1024 * 1024;  // memory for cached blocks
constexpr size_t kStdioBuffer = 8 * 1024;              // per open FILE
constexpr uint64_t kReadAheadMax = 8;                 // blocks (1 MiB) read ahead of a stream that keeps reading forward

uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Backing;
void queue_read_ahead(Backing* b, uint64_t idx);

// One file on the card, shared by every stream opened on it.
struct Backing {
    std::string path;
    FILE* file = nullptr;  // only used under io_mutex
    uint64_t size = 0;
    uint64_t blocks_total = 0;
    std::mutex io_mutex;

    struct Block {
        std::shared_ptr<uint8_t[]> data;
        uint32_t len = 0;
        bool ready = false;
        std::list<uint64_t>::iterator lru;
    };
    std::mutex mutex;  // guards everything below
    std::condition_variable loaded;
    std::unordered_map<uint64_t, Block> blocks;
    std::unordered_set<uint64_t> queued;  // waiting for the read-ahead thread
    std::list<uint64_t> lru;              // most recently used first (ready blocks only)
    uint64_t cached = 0;
    Stats st;

    // Block idx for a stream (loaded if needed); false on a read error.
    // `ahead`: blocks after it to have read in the background.
    bool fetch(uint64_t idx, uint64_t ahead, std::shared_ptr<uint8_t[]>& data, uint32_t& len) {
        std::unique_lock lk(mutex);
        st.reads++;
        auto it = blocks.find(idx);
        Block* b;
        if (it != blocks.end() && it->second.ready) {
            lru.splice(lru.begin(), lru, it->second.lru);  // most recently used
            b = &it->second;
        } else {
            const uint64_t t0 = now_ns();
            b = get(idx, lk, false);
            st.wait_ns += now_ns() - t0;  // the game's thread waited for the card
        }
        if (!b) return false;
        data = b->data;
        len = b->len;
        for (uint64_t n = idx + 1; n <= idx + ahead && n < blocks_total; n++)
            if (!blocks.count(n) && queued.insert(n).second) queue_read_ahead(this, n);
        return true;
    }

    // Read-ahead thread: load block idx unless a stream has already done so.
    void read_ahead(uint64_t idx) {
        std::unique_lock lk(mutex);
        queued.erase(idx);
        if (!blocks.count(idx)) get(idx, lk, true);
    }

    // The block, loaded if needed (lk is released while reading the card).
    Block* get(uint64_t idx, std::unique_lock<std::mutex>& lk, bool ahead) {
        for (;;) {
            auto it = blocks.find(idx);
            if (it != blocks.end()) {
                if (it->second.ready) {
                    lru.splice(lru.begin(), lru, it->second.lru);
                    return &it->second;
                }
                loaded.wait(lk);  // another thread is reading it
                continue;
            }
            blocks.emplace(idx, Block{});  // "loading": other readers wait
            lk.unlock();
            const uint64_t start = idx * kBlock;
            const uint32_t want = (uint32_t)std::min<uint64_t>(kBlock, size - start);
            std::shared_ptr<uint8_t[]> data(new (std::nothrow) uint8_t[want]);
            size_t got = 0;
            const uint64_t t0 = now_ns();
            if (data) {
                std::lock_guard io(io_mutex);
                if (fseeko(file, (off_t)start, SEEK_SET) == 0) got = std::fread(data.get(), 1, want, file);
            }
            const uint64_t t1 = now_ns();
            lk.lock();
            if (!data || got == 0) {
                blocks.erase(idx);
                loaded.notify_all();
                LOGW("OBB cache: could not read %u bytes at %llu", want, (unsigned long long)start);
                return nullptr;
            }
            auto& b = blocks[idx];
            b.data = std::move(data);
            b.len = (uint32_t)got;
            b.ready = true;
            lru.push_front(idx);
            b.lru = lru.begin();
            cached += got;
            st.block_loads++;
            if (ahead) st.read_ahead++;
            st.bytes_loaded += got;
            st.load_ns += t1 - t0;
            static const bool trace = std::getenv("NFSMW_OBB_TRACE") != nullptr;
            if (trace) {
                const cpu::GuestThread* t = ahead ? nullptr : cpu::current_or_null();
                LOGI("OBB cache: %s %u loaded block %llu (%.1f ms)", ahead ? "read-ahead" : "thread", t ? t->id : 0,
                     (unsigned long long)idx, (t1 - t0) / 1e6);
            }
            evict(idx);
            loaded.notify_all();
            return &blocks[idx];
        }
    }

    void evict(uint64_t keep) {
        while (cached > kCapacity && !lru.empty()) {
            const uint64_t victim = lru.back();
            if (victim == keep) break;
            lru.pop_back();
            auto it = blocks.find(victim);
            cached -= it->second.len;
            blocks.erase(it);
        }
    }
};

std::mutex g_mutex;
std::unordered_map<std::string, std::unique_ptr<Backing>> g_backings;

// The read-ahead thread and its queue (blocks are read in the order asked).
std::mutex g_ahead_mutex;
std::condition_variable g_ahead_cv;
std::deque<std::pair<Backing*, uint64_t>> g_ahead;
bool g_ahead_started = false;

void read_ahead_thread() {
    platform::set_thread_name("obb-read-ahead");
    for (;;) {
        std::pair<Backing*, uint64_t> job;
        {
            std::unique_lock lk(g_ahead_mutex);
            g_ahead_cv.wait(lk, [] { return !g_ahead.empty(); });
            job = g_ahead.front();
            g_ahead.pop_front();
        }
        job.first->read_ahead(job.second);
    }
}

// Called with b->mutex held (the read-ahead thread never takes a backing's
// mutex while holding g_ahead_mutex, so the lock order is always the same).
void queue_read_ahead(Backing* b, uint64_t idx) {
    std::lock_guard lk(g_ahead_mutex);
    if (!g_ahead_started) {
        g_ahead_started = true;
        // On a worker core, at a normal priority: it mostly waits for the card.
        platform::detach_thread(platform::spawn_thread(read_ahead_thread, 256 * 1024));
    }
    g_ahead.emplace_back(b, idx);
    g_ahead_cv.notify_one();
}

// One open FILE*. The game reads mostly a byte or a few at a time, so the
// stream keeps its current block and serves those reads without the lock
// (the FILE's own lock already serializes calls on one stream).
struct Stream {
    Backing* b;
    uint64_t pos = 0;
    std::shared_ptr<uint8_t[]> cur;
    uint64_t cur_idx = ~0ull;
    uint32_t cur_len = 0;
    uint32_t run = 0;  // blocks read one after the other
};

ssize_t c_read(void* cookie, char* buf, size_t n) {
    auto* s = static_cast<Stream*>(cookie);
    if (s->pos >= s->b->size) return 0;
    n = (size_t)std::min<uint64_t>(n, s->b->size - s->pos);
    size_t done = 0;
    while (done < n) {
        const uint64_t idx = s->pos / kBlock, in = s->pos % kBlock;
        if (idx != s->cur_idx) {
            // Reading forward block after block: read ahead, 1 block at first,
            // twice as many with each further block, up to kReadAheadMax.
            s->run = s->cur_idx != ~0ull && idx == s->cur_idx + 1 ? s->run + 1 : 0;
            const uint64_t ahead = s->run ? std::min<uint64_t>(kReadAheadMax, 1ull << std::min<uint32_t>(s->run - 1, 3)) : 0;
            if (!s->b->fetch(idx, ahead, s->cur, s->cur_len)) {
                s->cur_idx = ~0ull;
                break;
            }
            s->cur_idx = idx;
        }
        if (in >= s->cur_len) break;
        const size_t take = (size_t)std::min<uint64_t>(n - done, s->cur_len - in);
        std::memcpy(buf + done, s->cur.get() + in, take);
        done += take;
        s->pos += take;
    }
    return (ssize_t)done;
}
ssize_t c_write(void*, const char*, size_t) { return -1; }
// The offset type of the seek function differs between C libraries (glibc:
// off64_t, the Switch's newlib: off_t); take it from cookie_io_functions_t.
template <class F>
struct SeekOffset;
template <class R, class A, class B, class C>
struct SeekOffset<R (*)(A, B, C)> {
    using type = B;
};
using seek_off_ptr = SeekOffset<decltype(cookie_io_functions_t::seek)>::type;
int c_seek(void* cookie, seek_off_ptr off, int whence) {
    auto* s = static_cast<Stream*>(cookie);
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)s->pos : (int64_t)s->b->size;
    int64_t to = base + (int64_t)*off;
    if (to < 0) return -1;
    s->pos = (uint64_t)to;
    *off = to;
    return 0;
}
int c_close(void* cookie) {
    delete static_cast<Stream*>(cookie);
    return 0;
}

}  // namespace

FILE* open(const std::string& host_path) {
    Backing* b = nullptr;
    {
        std::lock_guard lk(g_mutex);
        auto& slot = g_backings[host_path];
        if (!slot) {
            FILE* f = std::fopen(host_path.c_str(), "rb");
            if (!f) {
                g_backings.erase(host_path);
                return nullptr;
            }
            setvbuf(f, nullptr, _IONBF, 0);  // whole blocks are read at once: no extra copy
            fseeko(f, 0, SEEK_END);
            auto nb = std::make_unique<Backing>();
            nb->path = host_path;
            nb->file = f;
            nb->size = (uint64_t)ftello(f);
            nb->blocks_total = (nb->size + kBlock - 1) / kBlock;
            slot = std::move(nb);
            LOGI("OBB cache: %s (%llu MiB) is read through a %llu MiB memory cache", host_path.c_str(),
                 (unsigned long long)(slot->size >> 20), (unsigned long long)(kCapacity >> 20));
        }
        b = slot.get();
    }
    cookie_io_functions_t io{};
    io.read = c_read;
    io.write = c_write;
    io.seek = c_seek;
    io.close = c_close;
    auto* s = new Stream{b};
    FILE* f = fopencookie(s, "rb", io);
    if (!f) {
        delete s;
        return nullptr;
    }
    // A small stdio buffer in front of the cache. Unbuffered (0.2.6), every
    // fgetc and every tiny fread went through the C library's refill, which
    // for an unbuffered stream first flushes every open output stream (the
    // Switch's newlib walks and locks all of them for each byte).
    static const bool unbuffered = std::getenv("NFSMW_OBB_UNBUFFERED") != nullptr;  // 0.2.6 behaviour, for comparisons
    if (unbuffered) setvbuf(f, nullptr, _IONBF, 0);
    else setvbuf(f, nullptr, _IOFBF, kStdioBuffer);
    return f;
}

Stats stats() {
    Stats total;
    std::lock_guard lk(g_mutex);
    for (auto& [path, b] : g_backings) {
        std::lock_guard blk(b->mutex);
        total.reads += b->st.reads;
        total.block_loads += b->st.block_loads;
        total.read_ahead += b->st.read_ahead;
        total.bytes_loaded += b->st.bytes_loaded;
        total.load_ns += b->st.load_ns;
        total.wait_ns += b->st.wait_ns;
        total.cached_bytes += b->cached;
    }
    return total;
}

std::string summary() {
    const Stats s = stats();
    if (!s.reads) return "";
    // Lookups the game's threads had to wait for the card on.
    const uint64_t misses = s.block_loads - s.read_ahead;
    char out[240];
    std::snprintf(out, sizeof out,
                  "OBB cache: %llu MiB, %llu block lookups (%.0f%% in memory), %llu card reads (%.1f MiB, %llu read "
                  "ahead, %llu ms), game waited %llu ms",
                  (unsigned long long)(s.cached_bytes >> 20), (unsigned long long)s.reads,
                  100.0 * (double)(s.reads > misses ? s.reads - misses : 0) / (double)s.reads,
                  (unsigned long long)s.block_loads, (double)s.bytes_loaded / (1 << 20),
                  (unsigned long long)s.read_ahead, (unsigned long long)(s.load_ns / 1000000),
                  (unsigned long long)(s.wait_ns / 1000000));
    return out;
}

}  // namespace hle::filecache
