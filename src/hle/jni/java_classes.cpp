// The Java side of the game (com.ea.ironmonkey.*, EAIO, a few android.*
// classes), re-implemented in C++ against the fake JNI.
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include "game/config.h"
#include "hle/hle.h"
#include "hle/jni/jni.h"
#include "hle/jni/text.h"
#include "game/paths.h"
#include "platform/platform.h"

namespace jni {

namespace {

using V = Value;

// ---- helpers ---------------------------------------------------------------
jref g_activity = 0, g_run_loop = 0, g_surface_view = 0, g_asset_manager = 0;

jref make_file(const std::string& path) {
    jref f = new_object("java/io/File");
    set_field(f, "path", V::L(new_string(path)));
    return f;
}

std::string asset_host_path(std::string p) {
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    return game::paths::assets_dir() + "/" + p;
}

// InputStream payload
struct Stream {
    std::ifstream f;
    s64 size = 0;
};

jref open_asset_stream(const std::string& path) {
    auto s = std::make_shared<Stream>();
    std::string hp = asset_host_path(path);
    s->f.open(hp, std::ios::binary);
    if (!s->f) {
        throw_new("java/io/FileNotFoundException", path);
        return 0;
    }
    s->f.seekg(0, std::ios::end);
    s->size = s->f.tellg();
    s->f.seekg(0);
    jref o = new_object("android/content/res/AssetManager$AssetInputStream");
    get(o)->native = s;
    LOGD("AssetManager.open(%s) -> %lld bytes", path.c_str(), (long long)s->size);
    return o;
}
Stream* stream(jref o) {
    Object* ob = get(o);
    return ob ? static_cast<Stream*>(ob->native.get()) : nullptr;
}

std::atomic<int> g_run_loop_state{0};

// One object per enum constant, like the JVM.
jref make_enum(const std::string& cls, const std::string& name, int ordinal) {
    static std::mutex m;
    static std::unordered_map<std::string, jref> cache;
    std::lock_guard lk(m);
    auto& r = cache[cls + "." + name];
    if (!r) {
        r = new_object(cls);
        set_field(r, "ordinal", V::I(ordinal));
        set_field(r, "name", V::L(new_string(name)));
    }
    return r;
}

}  // namespace

int run_loop_state() { return g_run_loop_state.load(); }
jref activity() { return g_activity; }
jref asset_manager() { return g_asset_manager; }
jref run_loop() { return g_run_loop; }
jref surface_view() { return g_surface_view; }

void register_java_classes() {
    // ---- android.os.Build ---------------------------------------------------
    set_static("android/os/Build", "MODEL", V::L(new_string("Nintendo Switch")));
    set_static("android/os/Build", "MANUFACTURER", V::L(new_string("Nintendo")));
    set_static("android/os/Build", "BRAND", V::L(new_string("nintendo")));
    set_static("android/os/Build", "DEVICE", V::L(new_string("nx")));
    set_static("android/os/Build", "PRODUCT", V::L(new_string("nx")));
    set_static("android/os/Build", "HARDWARE", V::L(new_string("tegra")));
    set_static("android/os/Build$VERSION", "SDK_INT", V::I(28));
    set_static("android/os/Build$VERSION", "RELEASE", V::L(new_string("9")));

    // ---- objects that exist for the whole run ----------------------------------
    g_activity = new_object("com/ea/ironmonkey/GameActivityMain");
    g_run_loop = new_object("com/ea/ironmonkey/RunLoop");
    g_surface_view = new_object("com/ea/ironmonkey/GameGLSurfaceView");
    g_asset_manager = new_object("android/content/res/AssetManager");
    set_static("com/ea/ironmonkey/GameActivityMain", "instance", V::L(g_activity));
    set_static("com/ea/EAMIO/StorageDirectory", "sActivity", V::L(g_activity));

    // ---- GameActivityMain -----------------------------------------------------------
    const char* GA = "com/ea/ironmonkey/GameActivityMain";
    def(GA, "getObbFullPath", "()Ljava/lang/String;", [](Call&) { return V::L(new_string(game::obb_path())); });
    def(GA, "isObbAssets", "()Z", [](Call&) { return V::Z(true); });
    def(GA, "isFullApkAssets", "()Z", [](Call&) { return V::Z(false); });
    def(GA, "useAssetsFileSystem", "()Z", [](Call&) { return V::Z(true); });
    def(GA, "isAssetsReady", "()Z", [](Call&) { return V::Z(true); });
    def(GA, "getAssetManager", "()Landroid/content/res/AssetManager;", [](Call&) { return V::L(g_asset_manager); });
    def(GA, "getAssets", "()Landroid/content/res/AssetManager;", [](Call&) { return V::L(g_asset_manager); });
    def(GA, "getDisplayMetrics", "()Landroid/util/DisplayMetrics;", [](Call&) {
        jref dm = new_object("android/util/DisplayMetrics");
        // Density scales with the height so the UI keeps the same proportions:
        // 720p handheld = 1.5 (240 dpi), 1080p docked = 2.25 (360 dpi).
        int h = game::settings().height;
        float density = 1.5f * (float)h / 720.f;
        set_field(dm, "widthPixels", V::I(game::settings().width));
        set_field(dm, "heightPixels", V::I(h));
        set_field(dm, "density", V::F(density));
        set_field(dm, "densityDpi", V::I((int)(160 * density)));
        set_field(dm, "scaledDensity", V::F(density));
        set_field(dm, "xdpi", V::F(160 * density));
        set_field(dm, "ydpi", V::F(160 * density));
        return V::L(dm);
    });
    def(GA, "GetApplicationVersion", "()Ljava/lang/String;", [](Call&) { return V::L(new_string(game::kVersionName)); });
    def(GA, "GetDeviceLocale", "()Ljava/lang/String;", [](Call&) { return V::L(new_string("EN-US")); });
    def(GA, "GetDefaultLanguage", "()Ljava/lang/String;", [](Call&) { return V::L(new_string("en")); });
    def(GA, "GetDeviceName", "()Ljava/lang/String;", [](Call&) { return V::L(new_string("Nintendo Switch")); });
    def(GA, "getOsVersion", "()Ljava/lang/String;", [](Call&) { return V::L(new_string("9")); });
    def(GA, "getTotalMemory", "()I", [](Call&) { return V::I(3072); });
    def(GA, "getPerformanceScore", "()F", [](Call&) { return V::F(game::settings().performance_score()); });
    def(GA, "getRunLoop", "()Lcom/ea/ironmonkey/RunLoop;", [](Call&) { return V::L(g_run_loop); });
    def(GA, "getAccelerometer", "()Lcom/ea/ironmonkey/Accelerometer;", [](Call&) { return V::L(0); });
    def(GA, "getGameGLSurfaceView", "()Lcom/ea/ironmonkey/GameGLSurfaceView;", [](Call&) { return V::L(g_surface_view); });
    def(GA, "disableSleepMode", "()V", [](Call&) { return V{}; });
    def(GA, "enableSleepMode", "()V", [](Call&) { return V{}; });
    def(GA, "isAnyMusicPlaying", "()Z", [](Call&) { return V::Z(false); });
    def(GA, "isAmazon", "()Z", [](Call&) { return V::Z(false); });
    def(GA, "getNotchesBoundingRects", "()[[I", [](Call&) { return V::L(new_object_array("[I", {})); });
    def(GA, "getUtcTime", "()J", [](Call&) {
        using namespace std::chrono;
        return V::J(duration_cast<hours>(system_clock::now().time_since_epoch()).count() / 24);
    });
    def(GA, "ShowMessage", "(Ljava/lang/String;[Ljava/lang/String;Z)V", [](Call& c) {
        LOGW("game message box: %s", c.s(0).c_str());
        return V{};
    });
    def(GA, "openURL", "(Ljava/lang/String;)V", [](Call& c) {
        LOGI("online services blocked: not opening %s", c.s(0).c_str());
        return V{};
    });
    def(GA, "openURLinBrowser", "(Ljava/lang/String;)V", [](Call& c) {
        LOGI("online services blocked: not opening %s", c.s(0).c_str());
        return V{};
    });
    def(GA, "needInstallWallpaper", "()Z", [](Call&) { return V::Z(false); });
    def(GA, "installWallpaper", "()V", [](Call&) { return V{}; });
    def(GA, "CallGC", "()V", [](Call&) { return V{}; });
    def(GA, "GetNaturalOrientation", "()I", [](Call&) { return V::I(0); });
    def(GA, "GetViewRoot", "()Landroid/view/ViewParent;", [](Call&) { return V::L(0); });
    def(GA, "getFilesDir", "()Ljava/io/File;", [](Call&) { return V::L(make_file(game::files_dir())); });
    def(GA, "getCacheDir", "()Ljava/io/File;", [](Call&) { return V::L(make_file(game::cache_dir())); });
    def(GA, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;", [](Call&) {
        return V::L(make_file(game::external_files_dir()));
    });
    def(GA, "getObbDir", "()Ljava/io/File;", [](Call&) { return V::L(make_file(game::obb_dir())); });
    def(GA, "getPackageName", "()Ljava/lang/String;", [](Call&) { return V::L(new_string(game::kPackage)); });
    def(GA, "getAssetSize", "(Ljava/lang/String;)I", [](Call& c) {
        struct stat st;
        if (::stat(asset_host_path(c.s(0)).c_str(), &st) == 0) return V::I(S_ISDIR(st.st_mode) ? -1 : (s32)st.st_size);
        return V::I(-2);
    });
    def(GA, "forEach", "(Ljava/lang/String;)[Ljava/lang/String;", [](Call& c) {
        std::vector<jref> names;
        if (DIR* d = ::opendir(asset_host_path(c.s(0)).c_str())) {
            while (auto* e = ::readdir(d))
                if (e->d_name[0] != '.') names.push_back(new_string(e->d_name));
            ::closedir(d);
        }
        return V::L(new_object_array("java/lang/String", names));
    });

    // ---- RunLoop / views -----------------------------------------------------------
    const char* RL = "com/ea/ironmonkey/RunLoop";
    def(RL, "start", "()V", [](Call&) { LOGI("RunLoop.start"); g_run_loop_state = 1; return V{}; });
    def(RL, "stop", "()V", [](Call&) { LOGI("RunLoop.stop"); g_run_loop_state = 0; return V{}; });
    def(RL, "join", "()V", [](Call&) { g_run_loop_state = 0; return V{}; });
    def(RL, "getState", "()I", [](Call&) { return V::I(g_run_loop_state.load()); });
    def("com/ea/ironmonkey/GameGLSurfaceView", "getWidth", "()I", [](Call&) { return V::I(game::settings().width); });
    def("com/ea/ironmonkey/GameGLSurfaceView", "getHeight", "()I", [](Call&) { return V::I(game::settings().height); });
    def("android/view/ViewRoot", "setProcessPositionEvents", "(Z)V", [](Call&) { return V{}; });

    // ---- files & environment ----------------------------------------------------------
    def("java/io/File", "getAbsolutePath", "()Ljava/lang/String;", [](Call& c) {
        Object* o = get(c.thiz);
        if (!o) return V::L(new_string("/"));
        auto it = o->fields.find("path");
        return it != o->fields.end() ? it->second : V::L(new_string("/"));
    });
    def("java/io/File", "getPath", "()Ljava/lang/String;", [](Call& c) {
        Object* o = get(c.thiz);
        return o ? o->fields["path"] : V::L(0);
    });
    def("android/os/Environment", "getExternalStorageDirectory", "()Ljava/io/File;", [](Call&) {
        return V::L(make_file(game::external_root()));
    });
    def("android/os/Environment", "getDataDirectory", "()Ljava/io/File;", [](Call&) { return V::L(make_file("/data")); });
    def("android/os/Environment", "getExternalStorageState", "()Ljava/lang/String;", [](Call&) {
        return V::L(new_string("mounted"));
    });
    const char* SD = "com/ea/EAMIO/StorageDirectory";
    def(SD, "GetInternalStorageDirectory", "()Ljava/lang/String;", [](Call&) { return V::L(new_string(game::files_dir())); });
    def(SD, "GetPrimaryExternalStorageDirectory", "()Ljava/lang/String;", [](Call&) {
        return V::L(new_string(game::external_files_dir()));
    });
    def(SD, "GetPrimaryExternalStorageDirectoryRoot", "()Ljava/lang/String;", [](Call&) {
        return V::L(new_string(game::external_root()));
    });
    def(SD, "GetPrimaryExternalStorageState", "()I", [](Call&) { return V::I(2); });

    // ---- assets -------------------------------------------------------------------
    const char* AM = "android/content/res/AssetManager";
    def(AM, "open", "(Ljava/lang/String;)Ljava/io/InputStream;", [](Call& c) { return V::L(open_asset_stream(c.s(0))); });
    def(AM, "openFd", "(Ljava/lang/String;)Landroid/content/res/AssetFileDescriptor;", [](Call& c) {
        struct stat st;
        std::string p = c.s(0);
        if (::stat(asset_host_path(p).c_str(), &st) != 0) {
            throw_new("java/io/FileNotFoundException", p);
            return V::L(0);
        }
        jref fd = new_object("android/content/res/AssetFileDescriptor");
        set_field(fd, "length", V::J(st.st_size));
        return V::L(fd);
    });
    def(AM, "list", "(Ljava/lang/String;)[Ljava/lang/String;", [](Call& c) {
        std::vector<jref> names;
        if (DIR* d = ::opendir(asset_host_path(c.s(0)).c_str())) {
            while (auto* e = ::readdir(d))
                if (e->d_name[0] != '.') names.push_back(new_string(e->d_name));
            ::closedir(d);
        }
        return V::L(new_object_array("java/lang/String", names));
    });
    const char* AFD = "android/content/res/AssetFileDescriptor";
    def(AFD, "getLength", "()J", [](Call& c) { return get(c.thiz) ? get(c.thiz)->fields["length"] : V::J(-1); });
    def(AFD, "getDeclaredLength", "()J", [](Call& c) { return get(c.thiz) ? get(c.thiz)->fields["length"] : V::J(-1); });
    def(AFD, "getStartOffset", "()J", [](Call&) { return V::J(0); });
    def(AFD, "close", "()V", [](Call&) { return V{}; });

    const char* IS = "java/io/InputStream";
    def(IS, "read", "([BII)I", [](Call& c) {
        Stream* s = stream(c.thiz);
        Object* arr = get(c.l(0));
        if (!s || !arr) return V::I(-1);
        s32 off = c.i(1), len = c.i(2);
        if (len <= 0) return V::I(0);
        s->f.read((char*)mem::host(arr->data + off), len);
        auto n = s->f.gcount();
        if (n <= 0) {
            s->f.clear();
            return V::I(-1);
        }
        return V::I((s32)n);
    });
    def(IS, "read", "([B)I", [](Call& c) {
        Stream* s = stream(c.thiz);
        Object* arr = get(c.l(0));
        if (!s || !arr) return V::I(-1);
        s->f.read((char*)mem::host(arr->data), arr->length);
        auto n = s->f.gcount();
        if (n <= 0) {
            s->f.clear();
            return V::I(-1);
        }
        return V::I((s32)n);
    });
    def(IS, "read", "()I", [](Call& c) {
        Stream* s = stream(c.thiz);
        if (!s) return V::I(-1);
        int ch = s->f.get();
        return V::I(ch == EOF ? -1 : ch);
    });
    def(IS, "skip", "(J)J", [](Call& c) {
        Stream* s = stream(c.thiz);
        if (!s) return V::J(0);
        s64 pos = s->f.tellg();
        s64 n = std::min<s64>(c.j(0), s->size - pos);
        s->f.seekg(pos + n);
        return V::J(n);
    });
    def(IS, "available", "()I", [](Call& c) {
        Stream* s = stream(c.thiz);
        if (!s) return V::I(0);
        return V::I((s32)(s->size - (s64)s->f.tellg()));
    });
    def(IS, "close", "()V", [](Call& c) {
        if (Object* o = get(c.thiz)) o->native.reset();
        return V{};
    });
    def(IS, "reset", "()V", [](Call& c) {
        if (Stream* s = stream(c.thiz)) s->f.seekg(0);
        return V{};
    });

    // ---- class loading: every class "exists" ---------------------------------------
    def("*", "getClassLoader", "()Ljava/lang/ClassLoader;", [](Call&) { return V::L(new_object("java/lang/ClassLoader")); });
    def("java/lang/ClassLoader", "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;", [](Call& c) {
        return V::L(find_class(c.s(0))->handle);
    });
    def("java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;", [](Call& c) {
        return V::L(find_class(c.s(0))->handle);
    });
    def("java/lang/Class", "getName", "()Ljava/lang/String;", [](Call& c) {
        Object* o = get(c.thiz);
        std::string n = o && o->as_class ? o->as_class->name : "";
        for (auto& ch : n)
            if (ch == '/') ch = '.';
        return V::L(new_string(n));
    });

    // ---- Java enums: objects carrying ordinal/name ------------------------------------
    def("*", "ordinal", "()I", [](Call& c) {
        Object* o = get(c.thiz);
        return o && o->fields.count("ordinal") ? o->fields["ordinal"] : V::I(0);
    });
    def("*", "name", "()Ljava/lang/String;", [](Call& c) {
        Object* o = get(c.thiz);
        return o && o->fields.count("name") ? o->fields["name"] : V::L(new_string(""));
    });

    // ---- EA Nimble: online services are blocked ------------------------------------
    // The SDK is told there is no network; analytics/tracking components do not
    // exist; network-change receivers are never registered.
    def("com/ea/nimble/Network", "getComponent", "()Lcom/ea/nimble/INetwork;", [](Call&) {
        static jref net = new_object("com/ea/nimble/INetwork");
        return V::L(net);
    });
    def("com/ea/nimble/INetwork", "getStatus", "()Lcom/ea/nimble/Network$Status;", [](Call&) {
        return V::L(make_enum("com/ea/nimble/Network$Status", "NONE", 1));
    });
    def("com/ea/nimble/INetwork", "isNetworkWifi", "()Z", [](Call&) { return V::Z(false); });
    def("com/ea/nimble/tracking/Tracking", "getComponent", "()Lcom/ea/nimble/tracking/ITracking;",
        [](Call&) { return V::L(0); });
    def("com/ea/nimble/Utility", "registerReceiver", "(Ljava/lang/String;Landroid/content/BroadcastReceiver;)V",
        [](Call&) { return V{}; });

    // ---- exceptions ----------------------------------------------------------------
    def("*", "getMessage", "()Ljava/lang/String;", [](Call& c) {
        Object* o = get(c.thiz);
        return o && o->fields.count("message") ? o->fields["message"] : V::L(new_string(""));
    });
    def("*", "toString", "()Ljava/lang/String;", [](Call& c) {
        Object* o = get(c.thiz);
        if (o && o->is_string) return V::L(c.thiz);
        return V::L(new_string(o && o->cls ? o->cls->name : "null"));
    });

    text::register_classes();
}

}  // namespace jni
