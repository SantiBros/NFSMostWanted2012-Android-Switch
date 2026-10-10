// Game-specific constants for Need for Speed: Most Wanted (Android 1.3.128).
#pragma once

#include <string>

namespace game {

constexpr const char* kPackage = "com.ea.games.nfs13_row";
constexpr int kVersionCode = 1003128;
constexpr const char* kVersionName = "1.3.128";

// Android paths the game is told about (mapped into the port's folder by game/paths.h).
inline std::string files_dir() { return std::string("/data/data/") + kPackage + "/files"; }
inline std::string cache_dir() { return std::string("/data/data/") + kPackage + "/cache"; }
inline std::string external_root() { return "/sdcard"; }
inline std::string external_files_dir() { return std::string("/sdcard/Android/data/") + kPackage + "/files"; }
inline std::string obb_dir() { return std::string("/sdcard/Android/obb/") + kPackage; }
inline std::string obb_name() { return "main." + std::to_string(kVersionCode) + "." + kPackage + ".obb"; }
inline std::string obb_path() { return obb_dir() + "/" + obb_name(); }

struct Settings {
    int width = 1280;   // Switch handheld resolution
    int height = 720;
    int max_frames = 0;           // 0 = run until closed
    int screenshot_every = 0;     // desktop: write a PNG every N frames
    std::string screenshot_dir = "screenshots";
    bool headless = false;
    int fps_limit = 0;            // desktop: cap the frame rate (0 = uncapped)
    bool size_forced = false;     // --width/--height given: ignore the platform's choice
    // GameActivityMain.calcPerformanceScore() for the Switch's CPU (4 cores at
    // 1785 MHz) and screen; the game derives its quality tier from this.
    float performance_score() const;
};
Settings& settings();

}  // namespace game
