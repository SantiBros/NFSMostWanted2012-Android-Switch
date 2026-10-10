// NFS Most Wanted 2012 (Android) loader: runs the game's 32-bit ARM code through
// Dynarmic and plays the part of the Android Activity around it.
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "core/cpu.h"
#include "core/elf.h"
#include "core/memory.h"
#include "game/audio.h"
#include "game/config.h"
#include "game/fixes.h"
#include "game/input.h"
#include "game/debug_menu.h"
#include "game/motion.h"
#include "game/paths.h"
#include "game/setup.h"
#include "game/native.h"
#include "game/perf.h"
#include "game/prefs.h"
#include "game/ui_nav.h"
#include "hle/gl_queue.h"
#include "hle/hle.h"
#include "hle/jni/jni.h"
#include "platform/platform.h"
#include "platform/thread.h"

#ifndef NFSMW_VERSION
#define NFSMW_VERSION "dev"
#endif

namespace jni {
int run_loop_state();
jref activity();
jref asset_manager();
jref run_loop();
}  // namespace jni

namespace game {
Settings& settings() {
    static Settings s;
    return s;
}
float Settings::performance_score() const { return perf::performance_score(width, height); }
}  // namespace game

namespace {

using jni::Value;

const char* kGA = "com/ea/ironmonkey/GameActivityMain";

Value native(const char* cls, const char* name, const char* sig, std::vector<Value> args = {}, jni::jref thiz = 0) {
    LOGI(">> %s.%s", cls, name);
    auto t0 = std::chrono::steady_clock::now();
    Value v = jni::call_native(cls, name, sig, thiz, args);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    LOGI("<< %s.%s (%.1f ms)", cls, name, ms);
    return v;
}

void parse_args(int argc, char** argv) {
    auto& s = game::settings();
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--frames") s.max_frames = std::stoi(next());
        else if (a == "--screenshot-every") s.screenshot_every = std::stoi(next());
        else if (a == "--screenshot-dir") s.screenshot_dir = next();
        else if (a == "--width") { s.width = std::stoi(next()); s.size_forced = true; }
        else if (a == "--height") { s.height = std::stoi(next()); s.size_forced = true; }
        else if (a == "--trace-gl") hle::gl_set_trace(true);
        else if (a == "--fps") s.fps_limit = std::stoi(next());
        else if (a == "--quiet") logging::g_min_level = logging::Info;
        else if (a == "--verbose") logging::g_min_level = logging::Verbose;
    }
}

}  // namespace

int main(int argc, char** argv) {
    game::paths::init(argc, argv);  // the port's folder: where the .nro was started from
    platform::startup_checks();
    platform::sample_set_main_thread();
    parse_args(argc, argv);
    auto& S = game::settings();
    // Switch: 1280x720 in handheld mode, 1920x1080 docked (desktop: --width/--height).
    if (!S.size_forced) platform::preferred_display_size(S.width, S.height);
    LOGI("display: %dx%d", S.width, S.height);
    LOGI("NFS Most Wanted 2012 loader %s starting (folder: %s)", NFSMW_VERSION, game::paths::root().c_str());
    game::paths::prepare();  // tidy up an older folder layout, create game/ and save/
    // First start: set the game up from the player's APK (in game/) when the
    // libraries, assets or OBB are missing, then start again on a clean screen.
    bool after_setup = false;
    for (int i = 1; i < argc; i++) after_setup = after_setup || std::strcmp(argv[i], "--after-setup") == 0;
    if (game::setup::run() && platform::finish_setup_screen(game::paths::self_nro(), !after_setup)) return 0;

    game::prefs::load();  // settings.ini: gyro steering, performance options
    mem::init();
    cpu::init(game::prefs::at_startup().direct_memory);
    hle::register_all();
    game::input::register_java();
    if (S.fps_limit > 0) game::perf::set_fps_override(S.fps_limit);  // desktop --fps (this run only)
    // Threaded GL: keep core 2 for the GL thread (Switch); the game's own
    // threads, most of which start before the GL context exists, use core 1.
    if (game::prefs::at_startup().gl_thread && !std::getenv("NFSMW_NO_GL_THREAD")) platform::reserve_core_for_gl(2);
    game::motion::load_settings();
    cpu::attach_current_thread(8 * 1024 * 1024);

    // Same order as GameActivityMain.onCreate's System.loadLibrary calls.
    const std::string lib = game::paths::lib_dir() + "/";
    for (const char* name : {"libfmodex.so", "libfmodevent.so", "libc++_shared.so", "libNimble.so", "libapp.so"}) {
        if (!elf::load(lib + name, name))
            fatal("Game files missing.\n\n%s was not found.\n\nPut your APK of the game (version 1.3.128, an "
                  ".apk or .xapk file) into the game folder:\n%s\nand start again: the port takes the files it "
                  "needs out of it by itself.",
                  game::paths::display(lib + name).c_str(), game::paths::display(game::paths::game_dir()).c_str());
    }
    {
        const std::string assets = game::paths::assets_dir();
        DIR* d = ::opendir(assets.c_str());
        bool any = false;
        while (dirent* e = d ? ::readdir(d) : nullptr) any = any || e->d_name[0] != '.';
        if (d) ::closedir(d);
        if (!any)
            fatal("Game files missing.\n\nThe folder %s (the assets of your APK) is missing or empty.\n\nPut your "
                  "APK of the game (version 1.3.128, an .apk or .xapk file) into the game folder:\n%s\nand start "
                  "again: the port takes the files it needs out of it by itself.",
                  game::paths::display(assets).c_str(), game::paths::display(game::paths::game_dir()).c_str());
    }
    // The game's data is the OBB file itself: a zip archive the game opens on
    // its own. It must be there unchanged (not extracted), in the game folder.
    {
        const std::string obb = game::paths::obb();
        FILE* f = std::fopen(obb.c_str(), "rb");
        if (!f)
            fatal("Game data missing.\n\nCopy your OBB file (%s), unchanged (do not unzip it), into the game "
                  "folder:\n%s\n\nIf your copy of the game is one .xapk file, putting that file into the game "
                  "folder is enough.",
                  game::obb_name().c_str(), game::paths::display(game::paths::game_dir()).c_str());
        LOGI("game data: %s", obb.c_str());
        unsigned char sig[2] = {};
        const size_t n = std::fread(sig, 1, sizeof sig, f);
        std::fclose(f);
        if (n != sizeof sig || sig[0] != 'P' || sig[1] != 'K')
            fatal("%s is not a valid OBB: it must be the original file (a zip archive), not extracted or converted",
                  game::paths::display(obb).c_str());
    }
    elf::relocate_all();
    // Debugging aid: NFSMW_PATCH="libapp.so+0x34013c=e5c43023,..." overwrites
    // guest words before anything runs (used to switch on the game's own
    // debug options, e.g. its AI driver, for automated test runs).
    if (const char* p = std::getenv("NFSMW_PATCH")) {
        std::string all = p;
        size_t i = 0;
        while (i < all.size()) {
            size_t j = all.find(',', i);
            if (j == std::string::npos) j = all.size();
            std::string item = all.substr(i, j - i);
            size_t plus = item.find('+'), eq = item.find('=');
            elf::Module* m = plus != std::string::npos ? elf::find(item.substr(0, plus)) : nullptr;
            if (m && eq != std::string::npos) {
                u32 off = (u32)std::stoul(item.substr(plus + 1, eq - plus - 1), nullptr, 16);
                u32 val = (u32)std::stoul(item.substr(eq + 1), nullptr, 16);
                mem::w32(m->base + off, val);
                LOGW("debug patch: %s+0x%X = %08X", m->name.c_str(), off, val);
            }
            i = j + 1;
        }
    }
    game::fixes::apply();
    game::audio::install();  // sound volume
    game::native::apply();  // native versions of the game's hottest OBB-reading code
    game::ui::init();  // controller navigation for the touch menus
    elf::run_initializers();

    for (elf::Module* m : elf::all()) {
        if (u32 onload = m->find_export("JNI_OnLoad")) {
            LOGI(">> %s JNI_OnLoad", m->name.c_str());
            u32 ver = (u32)cpu::current().call(onload, {jni::vm(), 0});
            LOGI("<< JNI_OnLoad -> 0x%X", ver);
        }
    }

    // Telemetry off: the game reads this flag at start-up (it writes "1" by default).
    {
        std::string flag = hle::map_path((game::files_dir() + "/enable.telemetry").c_str());
        if (FILE* f = std::fopen(flag.c_str(), "wb")) {
            std::fputs("0", f);
            std::fclose(f);
        }
    }

    // GameActivityMain.onCreate
    native("com/ea/EAIO/EAIO", "StartupNativeImpl",
           "(Landroid/content/res/AssetManager;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
           {Value::L(jni::asset_manager()), Value::L(jni::new_string("/data")),
            Value::L(jni::new_string(game::files_dir())), Value::L(jni::new_string(game::external_root()))});
    native("com/ea/EAMIO/StorageDirectory", "StartupNativeImpl", "()V");
    native(kGA, "nativeOnCreate", "()V", {}, jni::activity());
    // onStart / onResume
    native(kGA, "nativeOnStart", "()V", {}, jni::activity());
    native(kGA, "nativeOnResume", "()V", {}, jni::activity());
    if (!std::getenv("NFSMW_NO_AUDIO")) game::audio::start();  // FMODAudioDevice.start() in onResume

    // GL thread: surface callbacks
    if (!platform::gl_init(S.width, S.height)) fatal("could not create an OpenGL ES 2 context");
    game::perf::init(S.width, S.height);  // vsync / frame limit, clocks, 3D resolution
    // Threaded GL: the game's GL calls run on their own thread (Switch: core 2).
    if (game::prefs::at_startup().gl_thread && !std::getenv("NFSMW_NO_GL_THREAD")) {
        if (!hle::glq::start(2)) platform::reserve_core_for_gl(-1);
    }
    native(kGA, "nativeSurfaceCreated",
           "(Ljavax/microedition/khronos/opengles/GL10;Ljavax/microedition/khronos/egl/EGLConfig;)V",
           {Value::L(0), Value::L(0)}, jni::activity());
    native(kGA, "nativeSurfaceChanged", "(Ljavax/microedition/khronos/opengles/GL10;II)V",
           {Value::L(0), Value::I(S.width), Value::I(S.height)}, jni::activity());

    if (S.screenshot_every) ::mkdir(S.screenshot_dir.c_str(), 0755);
    bool started = false;
    auto t_start = std::chrono::steady_clock::now();
    // The game reads its view size once, at start-up, and cannot change it later
    // (verified: after a full context re-creation at the new size it keeps
    // drawing the old view). So the resolution is chosen at launch - 720p
    // handheld, 1080p docked - and kept if the console is docked or undocked
    // while playing. The Switch display hardware scales the picture to fit the
    // new screen; restarting the game gives the native resolution again.
    int mode_w = S.width, mode_h = S.height;
    int prof_frames = 0;
    double prof_tick = 0, prof_cpu = 0;
    for (int frame = 1; S.max_frames == 0 || frame <= S.max_frames; frame++) {
        game::perf::frame_begin();
        game::fixes::frame();
        game::audio::frame();
        if (!S.size_forced && frame % 30 == 0) {
            int want_w = S.width, want_h = S.height;
            platform::preferred_display_size(want_w, want_h);
            if (want_w != mode_w || want_h != mode_h) {
                mode_w = want_w;
                mode_h = want_h;
                if (want_w == S.width && want_h == S.height)
                    LOGI("display: back to %dx%d, native resolution again", S.width, S.height);
                else
                    LOGI("display: console switched to a %dx%d mode; still rendering %dx%d (scaled by the "
                         "console). Restart the game for native %dx%d.",
                         want_w, want_h, S.width, S.height, want_w, want_h);
            }
        }
        if (!started) {
            // GameActivityMain.onDrawFrame: splash until the native side has its context
            static auto glClearColor = (void (*)(float, float, float, float))platform::gl_get_proc("glClearColor");
            static auto glClear = (void (*)(unsigned))platform::gl_get_proc("glClear");
            hle::glq::call(glClearColor, 1.f, 1.f, 1.f, 1.f);
            hle::glq::call(glClear, 0x4100u);
            if (frame >= 3 && native(kGA, "nativeRestoreContext", "()Z", {}, jni::activity()).i()) {
                native(kGA, "nativeOnStart", "()V", {}, jni::activity());
                native(kGA, "nativeOnResume", "()V", {}, jni::activity());
                started = true;
            }
        } else if (jni::run_loop_state() == 1) {
            game::input::pump();
            // NFSMW_TRACE_WINDOW=N: summarize host calls made by the game loop every N frames
            static const int trace_window = std::getenv("NFSMW_TRACE_WINDOW") ? std::atoi(std::getenv("NFSMW_TRACE_WINDOW")) : 0;
            if (trace_window && frame % trace_window == 1) {
                cpu::trace_calls_begin();
                cpu::trace_all(true);
            }
            // per-frame cost split: total tick vs time inside the host GL driver
            static double acc_tick = 0, acc_gl = 0, acc_calls = 0;
            static int acc_n = 0;
            u64 gl0 = hle::gl_time_ns(), calls0 = hle::gl_call_count();
#if defined(__linux__)
            timespec cpu0;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu0);
#endif
            auto tk0 = std::chrono::steady_clock::now();
            game::ui::frame_begin();
            platform::sample_gate(true, &cpu::current().in_thunk);
            jni::call_native("com/ea/ironmonkey/RunLoop", "nativeOnRunLoopTick", "()V", jni::run_loop(), {});
            platform::sample_gate(false, nullptr);
            hle::gl_flush_state();  // vertex arrays the state filter held back, before the overlay
            double tick_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tk0).count();
            double gl_ms = (hle::gl_time_ns() - gl0) / 1e6;
            u64 calls = hle::gl_call_count() - calls0;
            if (prof_frames > 0) {  // debug command "profile": time per host function
                double cpu_ms = 0;
#if defined(__linux__)
                timespec cpu1;
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu1);
                cpu_ms = (cpu1.tv_sec - cpu0.tv_sec) * 1e3 + (cpu1.tv_nsec - cpu0.tv_nsec) / 1e6;
#endif
                if (prof_frames == 200) cpu::profile_begin(), prof_tick = 0, prof_cpu = 0;
                else prof_tick += tick_ms, prof_cpu += cpu_ms;
                if (--prof_frames == 0) {
                    cpu::profile_end(199, prof_tick);
                    LOGI("profile: main thread CPU time %.2f ms per frame of %.2f ms wall (the rest it was waiting "
                         "or preempted)", prof_cpu / 199, prof_tick / 199);
                }
            }
            game::perf::record_tick(tick_ms, gl_ms, calls);
            acc_tick += tick_ms;
            acc_gl += gl_ms;
            acc_calls += (double)calls;
            if (++acc_n == 100) {
                LOGD("perf: game tick %.2f ms/frame, of which GL driver %.2f ms (%.0f calls), game code + system "
                     "calls %.2f ms",
                     acc_tick / acc_n, acc_gl / acc_n, acc_calls / acc_n, (acc_tick - acc_gl) / acc_n);
                acc_tick = acc_gl = acc_calls = 0;
                acc_n = 0;
            }
            if (trace_window && frame % trace_window == 0) {
                char label[64];
                std::snprintf(label, sizeof label, "frames %d-%d", frame - trace_window + 1, frame);
                cpu::trace_calls_end(label);
                cpu::trace_all(false);
            }
        }
        if (started) {
            game::ui::update();
            game::ui::draw(S.width, S.height);
            game::input::draw_overlay(S.width, S.height);
            game::perf::draw_counter(S.width, S.height);
            game::debug_menu::draw(S.width, S.height);  // on top of everything
        }
        if (std::string dc = platform::debug_command(); !dc.empty()) {
            if (dc == "dumpui") game::ui::dump("debug command");
            if (dc == "threads") cpu::dump_threads();
            if (dc == "navoff") game::input::set_navigation_enabled(false);
            if (dc == "navon") game::input::set_navigation_enabled(true);
            if (dc == "fbtrace") hle::gl_fbtrace(2);
            if (dc == "profile") prof_frames = 200;
            if (dc == "sample") platform::sample_start();
        }
        if (hle::gl_fbtrace_left() > 0) { LOGI("fb: ---- swap ----"); hle::gl_fbtrace(hle::gl_fbtrace_left() - 1); }
        // Screenshots (desktop): of the finished frame, before the swap.
        if (std::string shot = platform::take_pending_screenshot(); !shot.empty()) {
            hle::glq::sync([&] { platform::gl_screenshot(shot); });
            LOGI("control: frame %d -> %s", frame, shot.c_str());
        }
        if (S.screenshot_every && frame % S.screenshot_every == 0) {
            char path[256];
            std::snprintf(path, sizeof path, "%s/frame_%05d.png", S.screenshot_dir.c_str(), frame);
            hle::glq::sync([&] { platform::gl_screenshot(path); });
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
            LOGI("frame %d (%.1f fps avg, %llu GL calls, heap %.1f MiB) -> %s", frame, frame / secs,
                 (unsigned long long)hle::gl_call_count(), mem::heap_bytes_in_use() / 1048576.0, path);
        }
        hle::glq::swap(&platform::gl_swap);  // on the GL thread when threaded
        if (!S.screenshot_every && frame % 500 == 0) {
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
            LOGI("frame %d at %.1f s", frame, secs);
        }
        static const int dump_every = std::getenv("NFSMW_DUMP_EVERY") ? std::atoi(std::getenv("NFSMW_DUMP_EVERY")) : 0;
        if (dump_every && frame % dump_every == 0) cpu::dump_threads();
        game::perf::frame_end();  // frame-rate limit, statistics
        if (platform::gl_should_close()) break;
    }
    cpu::dump_threads();
    LOGI("network calls refused during this run: %u", hle::blocked_network_attempts());
    LOGI("done");
    logging::flush();
    std::fflush(nullptr);
    std::_Exit(0);
}
