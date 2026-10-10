#include "game/setup.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/common.h"
#include "game/config.h"
#include "game/paths.h"
#include "platform/platform.h"

// stb_image's zlib decoder unpacks the APK's compressed ("deflate") entries.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "external/stb/stb_image.h"
#pragma GCC diagnostic pop

#ifndef NFSMW_VERSION
#define NFSMW_VERSION "dev"
#endif

namespace game::setup {

namespace {

const char* const kLibs[] = {"libapp.so", "libc++_shared.so", "libfmodex.so", "libfmodevent.so", "libNimble.so"};
const std::string kLibPrefix = "lib/armeabi-v7a/";
// What assets/ holds in the APK (1.3.128): to recognise its contents copied
// loose into game/.
const char* const kAssetItems[] = {"ext_data.bin", "int_data.bin", "licenseserver", "obb.size", "published",
                                   "splash.png"};
constexpr u32 kTestedLibappCrc = 0xD69D3C65;  // libapp.so of 1.3.128 (armeabi-v7a)
// Compressed entries are unpacked in memory; the libraries are 11 MB at most.
constexpr u64 kMaxInflate = 256ull << 20;

// ---- files ---------------------------------------------------------------------------
bool exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
bool is_dir(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool is_file(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
std::vector<std::string> list(const std::string& dir) {
    std::vector<std::string> names;
    if (DIR* d = ::opendir(dir.c_str())) {
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n != "." && n != "..") names.push_back(n);
        }
        ::closedir(d);
    }
    std::sort(names.begin(), names.end());
    return names;
}
std::string parent(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}
std::string base_name(const std::string& p) { return p.substr(p.find_last_of('/') + 1); }
void make_dirs(const std::string& p) {
    for (size_t i = 1; i < p.size(); i++)
        if (p[i] == '/' && p[i - 1] != ':') ::mkdir(p.substr(0, i).c_str(), 0755);
    ::mkdir(p.c_str(), 0755);
}
std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
bool starts_with(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }
std::string show(const std::string& p) { return paths::display(p); }

bool move(const std::string& from, const std::string& to) {
    if (!exists(from) || exists(to)) return false;
    make_dirs(parent(to));
    if (std::rename(from.c_str(), to.c_str()) != 0) {
        LOGW("setup: could not move %s to %s", show(from).c_str(), show(to).c_str());
        return false;
    }
    LOGI("setup: moved %s -> %s", show(from).c_str(), show(to).c_str());
    return true;
}
// Remove dir and its parents while they are empty, up to (not including) stop.
void prune_up(std::string dir, const std::string& stop) {
    while (!dir.empty() && dir != stop && starts_with(dir, stop + "/") && ::rmdir(dir.c_str()) == 0) dir = parent(dir);
}
// Files (and folders, if dirs is given) under dir, depth levels deep, skipping
// the folders in skip.
void walk(const std::string& dir, int depth, const std::vector<std::string>& skip, std::vector<std::string>& files,
          std::vector<std::string>* dirs = nullptr) {
    for (const std::string& n : list(dir)) {
        const std::string p = dir + "/" + n;
        if (is_dir(p)) {
            if (std::find(skip.begin(), skip.end(), p) != skip.end()) continue;
            if (dirs) dirs->push_back(p);
            if (depth > 0) walk(p, depth - 1, skip, files, dirs);
        } else {
            files.push_back(p);
        }
    }
}

// ---- what is missing -----------------------------------------------------------------
std::vector<std::string> missing_libs() {
    std::vector<std::string> out;
    for (const char* n : kLibs)
        if (!is_file(paths::lib_dir() + "/" + n)) out.push_back(n);
    return out;
}
bool assets_missing() { return !is_dir(paths::assets_dir()) || list(paths::assets_dir()).empty(); }
std::string obb_dest() { return paths::game_dir() + "/" + obb_name(); }
bool obb_missing() { return !is_file(obb_dest()); }
bool all_there() { return missing_libs().empty() && !assets_missing() && !obb_missing(); }

bool starts_with_pk(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    char sig[2] = {};
    const bool ok = std::fread(sig, 1, 2, f) == 2 && sig[0] == 'P' && sig[1] == 'K';
    std::fclose(f);
    return ok;
}

// ---- 1. files copied to the wrong place ----------------------------------------------
void fix_placement() {
    const std::string root = paths::root(), game = paths::game_dir(), lib = paths::lib_dir(),
                      assets = paths::assets_dir();
    const std::vector<std::string> skip_root = {game, paths::save_dir(), root + "/source"};

    // The libraries: still in lib/armeabi-v7a/, in game/armeabi-v7a/, loose in
    // game/ or in the port's folder...
    if (!missing_libs().empty()) {
        std::vector<std::string> files;
        walk(game, 5, {assets}, files);
        walk(root, 3, skip_root, files);
        for (const std::string& f : files) {
            const std::string name = base_name(f);
            if (std::find(std::begin(kLibs), std::end(kLibs), name) == std::end(kLibs) || parent(f) == lib) continue;
            if (move(f, lib + "/" + name)) prune_up(parent(f), parent(f).compare(0, game.size(), game) == 0 ? game : root);
        }
    }

    // The assets: the contents of assets/ loose in game/, or an assets folder
    // one level too deep (an extracted APK folder) or in the port's folder.
    if (assets_missing()) {
        if (is_file(game + "/int_data.bin") || is_dir(game + "/published")) {
            for (const char* item : kAssetItems) move(game + "/" + item, assets + "/" + item);
        } else {
            std::vector<std::string> files, dirs;
            walk(game, 3, {assets, lib}, files, &dirs);
            walk(root, 2, skip_root, files, &dirs);
            for (const std::string& d : dirs) {
                if (base_name(d) != "assets" || d == assets) continue;
                if (!is_file(d + "/int_data.bin") && !is_dir(d + "/published")) continue;
                if (is_dir(assets)) ::rmdir(assets.c_str());  // empty
                if (move(d, assets)) {
                    prune_up(parent(d), starts_with(d, game + "/") ? game : root);
                    break;
                }
            }
        }
    }

    // The OBB: in a subfolder (an unzipped bundle has it in
    // Android/obb/com.ea.games.nfs13_row/), in the port's folder, or renamed.
    if (obb_missing()) {
        std::vector<std::string> files, obbs;
        walk(game, 6, {assets, lib}, files);
        walk(root, 1, skip_root, files);
        for (const std::string& f : files)
            if (ends_with(lower(f), ".obb")) obbs.push_back(f);
        std::string pick;
        for (const std::string& f : obbs)
            if (base_name(f) == obb_name()) pick = f;
        if (pick.empty() && obbs.size() == 1 && starts_with_pk(obbs[0])) pick = obbs[0];
        if (!pick.empty() && move(pick, obb_dest()))
            prune_up(parent(pick), starts_with(pick, game + "/") ? game : root);
    }
}

// ---- zip archives (APK, XAPK) --------------------------------------------------------
u16 le16(const u8* p) { return (u16)(p[0] | p[1] << 8); }
u32 le32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
u64 le64(const u8* p) { return le32(p) | (u64)le32(p + 4) << 32; }

u32 crc32(u32 crc, const u8* p, size_t n) {
    static u32 table[256];
    static bool init = false;
    if (!init) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

struct Entry {
    std::string name;
    u16 method = 0, flags = 0;
    u32 crc = 0;
    u64 csize = 0, usize = 0, local = 0;
};

// A zip archive: a whole file, or a stored entry inside another archive (an
// APK inside an .xapk bundle), given as a byte range of the file.
class Archive {
public:
    std::string path;  // the file on the SD card
    std::string label;  // how the player knows it ("NFS.xapk", "NFS.xapk > base.apk")
    std::vector<Entry> entries;

    ~Archive() {
        if (f_) std::fclose(f_);
    }
    bool open(const std::string& file, u64 base = 0, u64 size = 0) {
        path = file;
        f_ = std::fopen(file.c_str(), "rb");
        if (!f_ || fseeko(f_, 0, SEEK_END) != 0) return false;
        const u64 fsize = (u64)ftello(f_);
        base_ = base;
        size_ = size ? size : fsize - std::min(base, fsize);
        if (size_ < 22 || base_ + size_ > fsize) return false;
        // End of central directory record, at most 64 KiB (comment) from the end.
        const u64 tail = std::min<u64>(size_, 65557);
        std::vector<u8> t(tail);
        if (!read_at(base_ + size_ - tail, t.data(), tail)) return false;
        long eocd = -1;
        for (long i = (long)tail - 22; i >= 0; i--)
            if (le32(&t[i]) == 0x06054b50) {
                eocd = i;
                break;
            }
        if (eocd < 0) return false;
        u64 count = le16(&t[eocd + 10]), cd_size = le32(&t[eocd + 12]), cd_off = le32(&t[eocd + 16]);
        if (count == 0xFFFF || cd_size == 0xFFFFFFFF || cd_off == 0xFFFFFFFF) {  // zip64
            if (eocd < 20 || le32(&t[eocd - 20]) != 0x07064b50) return false;
            u8 e[56];
            if (!read_at(base_ + le64(&t[eocd - 20 + 8]), e, sizeof e) || le32(e) != 0x06064b50) return false;
            count = le64(e + 32);
            cd_size = le64(e + 40);
            cd_off = le64(e + 48);
        }
        if (cd_off + cd_size > size_ || cd_size > (64u << 20)) return false;
        std::vector<u8> cd(cd_size);
        if (!read_at(base_ + cd_off, cd.data(), cd_size)) return false;
        size_t p = 0;
        for (u64 n = 0; n < count && p + 46 <= cd.size() && le32(&cd[p]) == 0x02014b50; n++) {
            Entry e;
            e.flags = le16(&cd[p + 8]);
            e.method = le16(&cd[p + 10]);
            e.crc = le32(&cd[p + 16]);
            e.csize = le32(&cd[p + 20]);
            e.usize = le32(&cd[p + 24]);
            const u16 nlen = le16(&cd[p + 28]), xlen = le16(&cd[p + 30]), clen = le16(&cd[p + 32]);
            e.local = le32(&cd[p + 42]);
            if (p + 46 + nlen + xlen > cd.size()) break;
            e.name.assign((const char*)&cd[p + 46], nlen);
            // zip64 sizes and offset, in that order, for the fields set to 0xFFFFFFFF
            for (size_t x = p + 46 + nlen; x + 4 <= p + 46 + nlen + xlen;) {
                const u16 id = le16(&cd[x]), len = le16(&cd[x + 2]);
                if (id == 0x0001) {
                    size_t q = x + 4;
                    const size_t end = std::min(q + len, cd.size());
                    if (e.usize == 0xFFFFFFFF && q + 8 <= end) e.usize = le64(&cd[q]), q += 8;
                    if (e.csize == 0xFFFFFFFF && q + 8 <= end) e.csize = le64(&cd[q]), q += 8;
                    if (e.local == 0xFFFFFFFF && q + 8 <= end) e.local = le64(&cd[q]), q += 8;
                }
                x += 4 + len;
            }
            p += 46 + nlen + xlen + clen;
            if (!e.name.empty() && e.name.back() != '/') entries.push_back(std::move(e));
        }
        return !entries.empty();
    }
    const Entry* find(const std::string& name) const {
        for (const Entry& e : entries)
            if (e.name == name) return &e;
        return nullptr;
    }
    // File offset of an entry's data (after its local header).
    bool data_start(const Entry& e, u64& off) {
        u8 h[30];
        if (!read_at(base_ + e.local, h, sizeof h) || le32(h) != 0x04034b50) return false;
        off = base_ + e.local + 30 + le16(h + 26) + le16(h + 28);
        return off + e.csize <= base_ + size_;
    }
    bool read_at(u64 off, void* buf, u64 n) {
        return fseeko(f_, (off_t)off, SEEK_SET) == 0 && std::fread(buf, 1, n, f_) == n;
    }

private:
    FILE* f_ = nullptr;
    u64 base_ = 0, size_ = 0;
};

using ProgressFn = std::function<void(u64 done, u64 total)>;

// Write one entry to dest (through dest.part, so a cut-off copy is never taken
// for a complete file), checking its CRC. Empty string on success, else why not.
std::string extract(Archive& a, const Entry& e, const std::string& dest, const ProgressFn& progress) {
    u64 off = 0;
    if ((e.flags & 1) || !a.data_start(e, off)) return a.label + " is damaged or encrypted (" + e.name + ")";
    if (e.method != 0 && e.method != 8) return a.label + " uses an unknown compression (" + e.name + ")";
    if (e.method == 8 && (e.usize > kMaxInflate || e.csize > kMaxInflate))
        return e.name + " in " + a.label + " is compressed and too large to unpack on the console";
    make_dirs(parent(dest));
    const std::string tmp = dest + ".part";
    std::remove(tmp.c_str());
    FILE* out = std::fopen(tmp.c_str(), "wb");
    if (!out) return "cannot write " + show(dest) + " (is the SD card full or read-only?)";
    std::string err;
    u32 crc = 0;
    if (e.method == 0) {
        std::vector<u8> buf(4u << 20);
        for (u64 done = 0; done < e.usize && err.empty();) {
            const u64 n = std::min<u64>(buf.size(), e.usize - done);
            if (!a.read_at(off + done, buf.data(), n)) err = "could not read " + a.label;
            else if (std::fwrite(buf.data(), 1, n, out) != n) err = "could not write " + show(dest) + ": the SD card may be full";
            else {
                crc = crc32(crc, buf.data(), n);
                done += n;
                progress(done, e.usize);
            }
        }
    } else {
        std::vector<char> in(e.csize), data(std::max<u64>(e.usize, 1));
        if (!a.read_at(off, in.data(), e.csize)) err = "could not read " + a.label;
        else if (e.usize && stbi_zlib_decode_noheader_buffer(data.data(), (int)e.usize, in.data(), (int)e.csize) !=
                                (int)e.usize)
            err = a.label + " is damaged (" + e.name + " does not unpack)";
        else if (std::fwrite(data.data(), 1, e.usize, out) != e.usize)
            err = "could not write " + show(dest) + ": the SD card may be full";
        else {
            crc = crc32(0, (const u8*)data.data(), e.usize);
            progress(e.usize, e.usize);
        }
    }
    if (std::fclose(out) != 0 && err.empty()) err = "could not write " + show(dest) + ": the SD card may be full";
    if (err.empty() && crc != e.crc) err = a.label + " is damaged (" + e.name + " fails its checksum): copy it again";
    if (err.empty() && std::rename(tmp.c_str(), dest.c_str()) != 0) err = "could not write " + show(dest);
    if (!err.empty()) std::remove(tmp.c_str());
    return err;
}

std::string human_size(u64 bytes) {
    char s[32];
    if (bytes >= (1ull << 30)) std::snprintf(s, sizeof s, "%.1f GB", bytes / 1073741824.0);
    else std::snprintf(s, sizeof s, "%llu MB", (unsigned long long)((bytes + (1 << 20) - 1) >> 20));
    return s;
}

// ---- the setup screen ----------------------------------------------------------------
struct Step {
    std::string title;
    int state = 0;  // 0 waiting, 1 working, 2 done
};

struct Screen {
    std::string source;
    std::vector<Step> steps;
    std::string doing;
    int percent = -1;
    std::string footer = "This happens only once. Please wait, and do not turn off the\n"
                         "  console or take out the SD card.";

    void draw() const {
        std::string t = "\n  NFS Most Wanted 2012 Loader " NFSMW_VERSION " - first start\n"
                        "  ----------------------------------------------------------\n\n"
                        "  Setting up the game from your own copy:\n    " +
                        source + "\n\n";
        for (const Step& s : steps)
            t += std::string("  ") + (s.state == 2 ? "[done] " : s.state == 1 ? "[....] " : "[    ] ") + s.title + "\n";
        t += "\n";
        if (!doing.empty()) t += "  " + doing + "\n";
        if (percent >= 0) {
            const int w = 50, fill = percent * w / 100;
            char pct[16];
            std::snprintf(pct, sizeof pct, "%3d%%", percent);
            t += "  [" + std::string(fill, '#') + std::string(w - fill, '-') + "] " + pct + "\n";
        }
        t += "\n  " + footer + "\n";
        platform::text_screen(t);
    }
};

[[noreturn]] void fail(const std::string& why) {
    platform::text_screen_close();
    fatal("Setting up the game did not work.\n\n%s", why.c_str());
}

// The player's APK or bundle in the game folder (or in the port's folder).
std::vector<std::string> find_packages() {
    std::vector<std::string> out;
    for (const std::string& dir : {paths::game_dir(), paths::root()})
        for (const std::string& n : list(dir)) {
            const std::string l = lower(n), p = dir + "/" + n;
            if (!is_file(p) || ends_with(l, ".part")) continue;
            if (ends_with(l, ".apk") || ends_with(l, ".xapk") || ends_with(l, ".apks") || ends_with(l, ".zip"))
                out.push_back(p);
        }
    // Plain APKs first.
    std::stable_sort(out.begin(), out.end(),
                     [](const std::string& a, const std::string& b) { return ends_with(lower(a), ".apk") && !ends_with(lower(b), ".apk"); });
    return out;
}

struct Source {
    Archive* archive = nullptr;
    const Entry* entry = nullptr;
};

}  // namespace

bool run() {
    if (all_there()) return false;
    LOGI("setup: game files missing (libraries: %zu, assets: %s, OBB: %s): looking for them",
         missing_libs().size(), assets_missing() ? "missing" : "ok", obb_missing() ? "missing" : "ok");
    fix_placement();
    if (all_there()) {
        LOGI("setup: all game files are in place now");
        return false;
    }

    // ---- 2. the player's APK / bundle
    std::vector<std::unique_ptr<Archive>> archives;
    std::vector<std::string> temp_files;  // compressed APKs inside a bundle, unpacked to read them
    Source libs[5];
    std::vector<Source> asset_entries;
    Source obb;
    bool any_package = false, arm64_only = false;
    std::string source_label;

    std::function<void(Archive&, int)> look_into = [&](Archive& a, int level) {
        bool has_v7a = false;
        for (const Entry& e : a.entries) {
            if (starts_with(e.name, kLibPrefix)) {
                has_v7a = true;
                for (int i = 0; i < 5; i++)
                    if (e.name == kLibPrefix + kLibs[i] && !libs[i].entry) libs[i] = {&a, &e};
            } else if (starts_with(e.name, "lib/arm64-v8a/")) {
                arm64_only = true;
            }
        }
        if (has_v7a) arm64_only = false;
        bool has_assets = false;
        for (const Entry& e : a.entries)
            if (starts_with(e.name, "assets/") && e.name.size() > 7) has_assets = true;
        if (has_assets && asset_entries.empty())
            for (const Entry& e : a.entries)
                if (starts_with(e.name, "assets/") && e.name.size() > 7) asset_entries.push_back({&a, &e});
        for (const Entry& e : a.entries)
            if (ends_with(lower(e.name), ".obb") && (!obb.entry || base_name(e.name) == obb_name())) obb = {&a, &e};
        if (has_v7a || has_assets) any_package = true;
        // A bundle (.xapk, .apks) has the APKs inside it; a plain APK (it has
        // the libraries) is not looked into further.
        if (level > 0 || has_v7a) return;
        for (const Entry& e : a.entries) {
            if (!ends_with(lower(e.name), ".apk")) continue;
            auto inner = std::make_unique<Archive>();
            inner->label = a.label + " > " + base_name(e.name);
            u64 off = 0;
            bool ok = false;
            if (e.method == 0 && a.data_start(e, off)) {
                ok = inner->open(a.path, off, e.csize);
            } else if (e.method == 8) {
                const std::string tmp = paths::game_dir() + "/.setup-" + std::to_string(temp_files.size()) + ".apk";
                const std::string err = extract(a, e, tmp, [](u64, u64) {});
                if (err.empty()) {
                    temp_files.push_back(tmp);
                    ok = inner->open(tmp);
                } else {
                    LOGW("setup: %s", err.c_str());
                }
            }
            if (!ok) continue;
            LOGI("setup: %s (%zu entries)", inner->label.c_str(), inner->entries.size());
            look_into(*inner, level + 1);
            archives.push_back(std::move(inner));
        }
    };

    for (const std::string& p : find_packages()) {
        auto a = std::make_unique<Archive>();
        a->label = base_name(p);
        if (!a->open(p)) {
            LOGI("setup: %s is not a zip archive, skipped", show(p).c_str());
            continue;
        }
        LOGI("setup: looking into %s (%zu entries)", show(p).c_str(), a->entries.size());
        const size_t libs_before = std::count_if(std::begin(libs), std::end(libs), [](const Source& s) { return s.entry; });
        const bool assets_before = !asset_entries.empty(), obb_before = obb.entry != nullptr;
        look_into(*a, 0);
        const size_t libs_after = std::count_if(std::begin(libs), std::end(libs), [](const Source& s) { return s.entry; });
        if (source_label.empty() &&
            (libs_after > libs_before || (!assets_before && !asset_entries.empty()) || (!obb_before && obb.entry)))
            source_label = starts_with(p, paths::root() + "/") ? p.substr(paths::root().size() + 1) : show(p);
        archives.push_back(std::move(a));
    }

    // What to write.
    struct Item {
        Source src;
        std::string dest;
        int step;
    };
    std::vector<Item> items;
    Screen screen;
    screen.source = source_label;
    const std::vector<std::string> need_libs = missing_libs();
    if (!need_libs.empty()) {
        bool all = true;
        for (const std::string& n : need_libs) {
            const int i = (int)(std::find(std::begin(kLibs), std::end(kLibs), n) - std::begin(kLibs));
            if (libs[i].entry) items.push_back({libs[i], paths::lib_dir() + "/" + n, 0});
            else all = false;
        }
        if (!all && arm64_only)
            fail("Your APK has only the 64-bit (arm64-v8a) build of the game. The port runs the 32-bit build "
                 "(armeabi-v7a) of version 1.3.128: use an APK that has the folder lib/armeabi-v7a.");
        if (!all && any_package)
            fail("Your APK does not have all 5 game libraries (lib/armeabi-v7a/: libapp.so, libc++_shared.so, "
                 "libfmodex.so, libfmodevent.so, libNimble.so). Use the APK of Need for Speed Most Wanted 1.3.128.");
    }
    if (!items.empty()) screen.steps.push_back({"Game libraries (" + std::to_string(items.size()) + " files)"});
    if (assets_missing() && !asset_entries.empty()) {
        const int step = (int)screen.steps.size();
        for (const Source& s : asset_entries) {
            const std::string dest = paths::assets_dir() + "/" + s.entry->name.substr(7);
            if (!exists(dest)) items.push_back({s, dest, step});
        }
        screen.steps.push_back({"Game assets (" + std::to_string(asset_entries.size()) + " files)"});
    }
    if (obb_missing() && obb.entry) {
        if (obb.entry->method != 0)
            fail("The OBB inside " + obb.archive->label + " is compressed, so it cannot be copied on the console. "
                 "Unzip " + obb.archive->label + " on a computer and copy the .obb file into the game folder.");
        items.push_back({obb, obb_dest(), (int)screen.steps.size()});
        screen.steps.push_back({"Game data (OBB, " + human_size(obb.entry->usize) + ")"});
    }
    if (items.empty()) {
        LOGI("setup: no APK in the game folder that has what is missing");
        for (const std::string& t : temp_files) std::remove(t.c_str());
        return false;
    }

    // Enough room on the SD card?
    u64 need = 16ull << 20;
    for (const Item& it : items) need += it.src.entry->usize;
    struct statvfs vfs;
    if (::statvfs(paths::root().c_str(), &vfs) == 0) {
        const u64 free_bytes = (u64)vfs.f_bavail * (vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize);
        if (free_bytes < need)
            fail("Not enough free space on the SD card: setting up needs " + human_size(need) + ", " +
                 human_size(free_bytes) + " are free.");
    }

    // Write everything, with the progress on screen.
    LOGI("setup: writing %zu files from %s", items.size(), source_label.c_str());
    if (libs[0].entry && libs[0].entry->crc != kTestedLibappCrc)
        LOGW("setup: libapp.so (crc32 %08X) is not the tested 1.3.128 build; it may still work", libs[0].entry->crc);
    for (size_t i = 0; i < items.size(); i++) {
        const Item& it = items[i];
        for (size_t s = 0; s < screen.steps.size(); s++)
            screen.steps[s].state = (int)s < it.step ? 2 : (int)s == it.step ? 1 : 0;
        const bool big = it.src.entry->usize > (32u << 20);
        screen.doing = big ? "Copying " + base_name(it.dest) + "..." : "Writing " + base_name(it.dest) + "...";
        screen.percent = big ? 0 : -1;
        screen.draw();
        int last = -1;
        const std::string err = extract(*it.src.archive, *it.src.entry, it.dest, [&](u64 done, u64 total) {
            if (!big || !total) return;
            const int pct = (int)(done * 100 / total);
            if (pct == last) return;
            last = pct;
            screen.percent = pct;
            screen.draw();
        });
        if (!err.empty()) {
            for (const std::string& t : temp_files) std::remove(t.c_str());
            fail(err);
        }
        LOGI("setup: wrote %s (%llu bytes)", show(it.dest).c_str(), (unsigned long long)it.src.entry->usize);
    }
    for (const std::string& t : temp_files) std::remove(t.c_str());

    for (Step& s : screen.steps) s.state = 2;
    screen.percent = -1;
    screen.doing = "Done!";
    screen.footer = "Your " + base_name(source_label) + " is not needed any more:\n"
                    "  you can delete it from the game folder to free space.";
    if (!all_there())
        screen.footer = "Some game files are still missing; the next screen says which.";
    screen.draw();
    LOGI("setup: finished");
    return true;
}

}  // namespace game::setup
