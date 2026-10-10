#include "game/paths.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/common.h"
#include "game/config.h"

#ifndef NFSMW_VERSION
#define NFSMW_VERSION "0.0.0"
#endif

namespace game::paths {

namespace {
std::string g_root;
std::string g_self;  // the .nro that is running (file name), if known

#if defined(__SWITCH__)
constexpr const char* kDefaultRoot = "sdmc:/switch/NFSMostWanted2012";
// The port's folders under its earlier names: "NFSMostWanted" (0.1.0 - 0.2.0,
// before the rename that tells it apart from the Most Wanted (2005) port) and
// "nfsmw" (test builds). Their files are moved into this folder at start-up.
const char* const kPreviousRoots[] = {"sdmc:/switch/NFSMostWanted", "sdmc:/switch/nfsmw"};
// /proc and /sys files the game reads (CPU count, memory): built into the .nro.
const std::string kSystemDir = "romfs:/system";
#elif defined(NFSMW_ROMFS_DIR)
const std::string kSystemDir = std::string(NFSMW_ROMFS_DIR) + "/system";
#else
const std::string kSystemDir = "res/romfs/system";
#endif

bool exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
bool is_dir(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
void make_dirs(const std::string& p) {
    for (size_t i = 1; i < p.size(); i++)
        if (p[i] == '/' && p[i - 1] != ':') ::mkdir(p.substr(0, i).c_str(), 0755);
    ::mkdir(p.c_str(), 0755);
}
std::string parent(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}
bool starts_with(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

// ---- tidying up earlier layouts ------------------------------------------------------
// Only moves (renames on the same card) and only when the destination does not
// exist yet, so nothing is ever overwritten.
void move(const std::string& from, const std::string& to) {
    if (!exists(from) || exists(to)) return;
    make_dirs(parent(to));
    if (std::rename(from.c_str(), to.c_str()) == 0) LOGI("folders: moved %s -> %s", display(from).c_str(), display(to).c_str());
    else LOGW("folders: could not move %s to %s", display(from).c_str(), display(to).c_str());
}
void remove_file(const std::string& p) {
    if (exists(p) && std::remove(p.c_str()) == 0) LOGI("folders: removed %s", display(p).c_str());
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
    return names;
}
// Delete a folder and everything in it (only for files the port made itself).
void remove_tree(const std::string& dir) {
    for (const std::string& n : list(dir)) {
        const std::string p = dir + "/" + n;
        if (is_dir(p)) remove_tree(p);
        else std::remove(p.c_str());
    }
    ::rmdir(dir.c_str());
}
// Remove the empty folders in a tree (and the tree itself if it ends up empty).
void prune(const std::string& dir) {
    for (const std::string& n : list(dir))
        if (is_dir(dir + "/" + n)) prune(dir + "/" + n);
    ::rmdir(dir.c_str());  // fails, harmlessly, while something is left
}
void files_in(const std::string& dir, std::vector<std::string>& out) {
    for (const std::string& n : list(dir)) {
        const std::string p = dir + "/" + n;
        if (is_dir(p)) files_in(p, out);
        else out.push_back(p);
    }
}
// Move every file under old_dir (an earlier copy of an Android folder) to
// where that Android path belongs now.
void migrate_android_tree(const std::string& old_dir, const std::string& android_dir) {
    if (!is_dir(old_dir)) return;
    std::vector<std::string> files;
    files_in(old_dir, files);
    for (const std::string& f : files) move(f, from_android(android_dir + f.substr(old_dir.size())));
    prune(old_dir);
}
// Our own guide files from earlier releases (all in README.txt now).
void remove_old_doc(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return;
    char head[256] = {};
    size_t n = std::fread(head, 1, sizeof head - 1, f);
    std::fclose(f);
    head[n] = 0;
    if (std::strstr(head, "NFS MOST WANTED FOR NINTENDO SWITCH") || std::strstr(head, "SAVE FOLDER - YOUR PROGRESS") ||
        std::strstr(head, "Put your OBB file in THIS folder") || std::strstr(head, "Copy your OBB file into THIS folder"))
        remove_file(p);
}

// "1.2.3" -> {1, 2, 3}; false if the text is not a version.
bool parse_version(const std::string& v, int out[3]) {
    return std::sscanf(v.c_str(), "%d.%d.%d", &out[0], &out[1], &out[2]) == 3;
}
// An older version of the port: its .nro under an earlier name (nfsmw.nro,
// NFSMostWanted.nro, NFSMostWanted-<version>.nro), or NFSMostWanted2012-<version>.nro
// with a lower version than this one. Never the .nro that is running, never a
// newer version, and nothing at all when the running .nro is not known.
bool is_old_port_nro(const std::string& name) {
    if (g_self.empty() || name == g_self || name.size() < 5 || name.compare(name.size() - 4, 4, ".nro") != 0) return false;
    if (name == "nfsmw.nro" || name == "NFSMostWanted.nro" || starts_with(name, "NFSMostWanted-")) return true;
    const std::string prefix = "NFSMostWanted2012-";
    int theirs[3], ours[3];
    if (!starts_with(name, prefix) || !parse_version(name.substr(prefix.size(), name.size() - 4 - prefix.size()), theirs) ||
        !parse_version(NFSMW_VERSION, ours))
        return false;
    return std::lexicographical_compare(theirs, theirs + 3, ours, ours + 3);
}

std::vector<std::string> previous_roots() {
#if defined(__SWITCH__)
    return {std::begin(kPreviousRoots), std::end(kPreviousRoots)};
#else
    const char* p = std::getenv("NFSMW_PREVIOUS_ROOT");  // for testing the move on the desktop
    return p ? std::vector<std::string>{p} : std::vector<std::string>{};
#endif
}

// A folder of an earlier name is only touched when it holds this port's own
// files (the game's libraries or OBB, or its saves): another homebrew's folder
// with the same name is left alone.
bool holds_this_port(const std::string& d) {
    const std::string obb_file = obb_name(), pkg = kPackage;
    for (const std::string& p : {d + "/game/lib/libapp.so", d + "/game/" + obb_file, d + "/lib/libapp.so",
                                 d + "/" + obb_file, d + "/fs/sdcard/Android/obb/" + pkg, d + "/save/progress",
                                 d + "/save/internal", d + "/fs/data/data/" + pkg})
        if (exists(p)) return true;
    return false;
}

// Move from into to: a file or folder that is not there yet is moved whole;
// folders that exist on both sides are merged. Nothing is overwritten.
void merge(const std::string& from, const std::string& to) {
    if (!exists(from)) return;
    if (!exists(to)) {
        move(from, to);
        return;
    }
    if (is_dir(from) && is_dir(to)) {
        for (const std::string& n : list(from)) merge(from + "/" + n, to + "/" + n);
        ::rmdir(from.c_str());  // fails, harmlessly, while something is left
    }
}

// The port's folder under an earlier name: move the player's game files,
// progress and settings into this folder, then remove the old version's own
// files (its .nro, guides, log, source code) and the folder if that empties it.
// Same folder, or one inside the other (FAT names ignore case).
bool overlaps(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    if (::strncasecmp(a.c_str(), b.c_str(), n) != 0) return false;
    return a.size() == b.size() || (a.size() > n ? a[n] : b[n]) == '/';
}

void adopt_previous_folders() {
    for (const std::string& prev : previous_roots()) {
        if (overlaps(prev, g_root) || !is_dir(prev) || !holds_this_port(prev)) continue;
        LOGI("folders: moving your game files and progress from %s (the port's folder before it was renamed)",
             display(prev).c_str());
        for (const char* item : {"game", "save", "settings.ini", "lib", "apk", "fs"}) merge(prev + "/" + item, g_root + "/" + item);
        merge(prev + "/" + obb_name(), g_root + "/" + obb_name());
        for (const std::string& n : list(prev)) {
            const std::string p = prev + "/" + n;
            if (is_old_port_nro(n) || n == "log.txt" || n == "nfsmw.log") remove_file(p);
        }
        remove_old_doc(prev + "/README.txt");
        remove_old_doc(prev + "/LICENSES.txt");
        remove_old_doc(prev + "/CHANGELOG.txt");
        remove_old_doc(prev + "/PUT YOUR OBB FILE HERE.txt");
        if (exists(prev + "/source/src/game/paths.cpp") && exists(prev + "/source/CMakeLists.txt")) {
            remove_tree(prev + "/source");
            LOGI("folders: removed the old version's source code (%s/source)", display(prev).c_str());
        }
        prune(prev);
        if (exists(prev))
            LOGW("folders: %s still has files (copies of files this folder already has, or files the port did not "
                 "make); check it and delete it if you no longer need it",
                 display(prev).c_str());
        else
            LOGI("folders: removed the old folder %s", display(prev).c_str());
    }
}

void migrate() {
    // The .nro of an earlier version next to this one: the Homebrew Menu could
    // keep starting it.
    for (const std::string& n : list(g_root))
        if (is_old_port_nro(n)) remove_file(g_root + "/" + n);

    const std::string r = g_root, pkg = kPackage, obb_file = obb_name(), fs = r + "/fs", save = save_dir();
    const std::string old_docs[] = {r + "/PUT YOUR OBB FILE HERE.txt", r + "/CHANGELOG.txt", game_dir() + "/README.txt",
                                    save + "/README.txt",
                                    fs + "/sdcard/Android/obb/" + pkg + "/PUT YOUR OBB FILE HERE.txt"};
    bool any = exists(r + "/lib") || exists(r + "/apk") || exists(fs) || exists(r + "/" + obb_file) ||
               exists(save + "/internal") || exists(save + "/external") || exists(save + "/system") ||
               exists(r + "/nfsmw.log");
    for (const std::string& d : old_docs) any = any || exists(d);
    if (!any) return;
    LOGI("folders: tidying up an earlier folder layout");

    // The APK's files and the OBB -> game/
    move(r + "/lib", lib_dir());
    move(r + "/apk/assets", assets_dir());
    if (exists(r + "/lib") && exists(lib_dir())) {  // already in game/: the old copies are not needed
        for (const char* f : {"libapp.so", "libc++_shared.so", "libfmodex.so", "libfmodevent.so", "libNimble.so"})
            remove_file(r + "/lib/" + f);
        prune(r + "/lib");
    }
    if (exists(r + "/apk/assets") && exists(assets_dir())) {
        remove_tree(r + "/apk/assets");
        LOGI("folders: removed the old copy of the APK's assets (apk/assets)");
    }
    prune(r + "/apk");
    const std::string old_obb_in_fs = fs + "/sdcard/Android/obb/" + pkg + "/" + obb_file;
    move(r + "/" + obb_file, game_dir() + "/" + obb_file);
    move(old_obb_in_fs, game_dir() + "/" + obb_file);
    for (const std::string& old : {r + "/" + obb_file, old_obb_in_fs})
        if (exists(old))
            LOGW("folders: %s is an older copy of the OBB and is no longer used (game/ has it); you can delete it",
                 display(old).c_str());

    // Earlier copies of the game's Android storage -> save/progress, save/settings
    migrate_android_tree(fs + "/data/data/" + pkg, "/data/data/" + pkg);
    migrate_android_tree(fs + "/sdcard/Android/data/" + pkg, "/sdcard/Android/data/" + pkg);
    migrate_android_tree(save + "/internal", "/data/data/" + pkg);
    migrate_android_tree(save + "/external", "/sdcard/Android/data/" + pkg);
    // The /proc and /sys files are built into the .nro now.
    if (exists(save + "/system")) {
        remove_tree(save + "/system");
        LOGI("folders: removed save/system (built into the .nro now)");
    }
    if (exists(fs + "/proc")) remove_tree(fs + "/proc");
    if (exists(fs + "/sys")) remove_tree(fs + "/sys");
    for (const std::string& d : old_docs) remove_old_doc(d);
    prune(fs);
    // Anything else the game kept there (not an old OBB copy: that stays for
    // the player to delete, as the warning above says).
    if (!exists(old_obb_in_fs)) move(fs, settings_dir() + "/other");
    remove_file(r + "/nfsmw.log");  // the log is log.txt now
}
}  // namespace

void init(int argc, char** argv) {
#if defined(__SWITCH__)
    // The Homebrew Menu (and launchers like it) pass the .nro's path as argv[0].
    std::string self = argc > 0 && argv && argv[0] ? argv[0] : "";
    if (!self.empty() && self[0] == '/') self = "sdmc:" + self;
    const size_t slash = self.find_last_of('/');
    if (self.size() > 4 && self.compare(self.size() - 4, 4, ".nro") == 0 && slash != std::string::npos &&
        self.find(":/") != std::string::npos) {
        g_root = self.substr(0, slash);
        g_self = self.substr(slash + 1);
    } else {
        g_root = kDefaultRoot;
        if (!is_dir(kDefaultRoot))
            for (const char* p : kPreviousRoots)
                if (is_dir(p)) {
                    g_root = p;
                    break;
                }
    }
#else
    (void)argc, (void)argv;
    const char* r = std::getenv("NFSMW_ROOT");
    g_root = r ? r : "gamedata";
    if (const char* s = std::getenv("NFSMW_SELF")) g_self = s;  // tests: the .nro name to keep
#endif
    while (g_root.size() > 1 && g_root.back() == '/') g_root.pop_back();
}

const std::string& root() {
    if (g_root.empty()) init(0, nullptr);
    return g_root;
}

std::string self_nro() { return g_self.empty() ? std::string() : root() + "/" + g_self; }
std::string game_dir() { return root() + "/game"; }
std::string lib_dir() { return game_dir() + "/lib"; }
std::string assets_dir() { return game_dir() + "/assets"; }
std::string save_dir() { return root() + "/save"; }
std::string progress_dir() { return save_dir() + "/progress"; }
std::string settings_dir() { return save_dir() + "/settings"; }
std::string settings_file() { return root() + "/settings.ini"; }
std::string log_file() { return root() + "/log.txt"; }

std::string obb() {
    const std::string in_game = game_dir() + "/" + obb_name();
    if (exists(in_game)) return in_game;
    // Where earlier versions kept it (before prepare() moved it).
    for (const std::string& p : {root() + "/" + obb_name(), root() + "/fs" + obb_path()})
        if (exists(p)) return p;
    return in_game;
}

std::string display(const std::string& host_path) {
    std::string p = host_path;
    if (starts_with(p, "sdmc:/")) p = p.substr(6);
    return p;
}

std::string from_android(const std::string& p) {
    std::string rest;
    auto under = [&](const std::string& prefix) {
        if (p == prefix) {
            rest.clear();
            return true;
        }
        if (p.size() > prefix.size() && starts_with(p, prefix) && p[prefix.size()] == '/') {
            rest = p.substr(prefix.size());
            return true;
        }
        return false;
    };
    // A file in the app's own storage (internal or external): the career save
    // goes to progress/, the rest to settings/, without the app's subfolders.
    auto app_file = [&](std::string r) {
        const std::string name = r.substr(r.find_last_of('/') + 1);
        if (starts_with(name, "nfstr_save")) return progress_dir() + "/" + name;
        for (const char* sub : {"/files/var1", "/files/var", "/files"}) {
            const std::string s = sub;
            if (r == s) return settings_dir();
            if (starts_with(r, s + "/")) {
                r = r.substr(s.size());
                break;
            }
        }
        return settings_dir() + r;
    };
    const std::string pkg = kPackage;
    if (under("/sdcard/Android/obb/" + pkg)) return rest == "/" + obb_name() ? obb() : game_dir() + rest;
    if (under("/data/data/" + pkg)) return app_file(rest);
    if (under("/sdcard/Android/data/" + pkg)) return app_file(rest);
    if (under("/proc")) return kSystemDir + "/proc" + rest;
    if (under("/sys")) return kSystemDir + "/sys" + rest;
    return settings_dir() + "/other" + p;
}

void prepare() {
    adopt_previous_folders();
    migrate();
    make_dirs(game_dir());
    make_dirs(progress_dir());
    make_dirs(settings_dir());
}

}  // namespace game::paths
