#pragma once

#include <string>

namespace game::audio {

// Start/stop the FMOD mixing thread (FMODAudioDevice.start()/stop() on Android).
void start();
void stop();

// Hook FMOD (after the libraries are loaded, before they run) so that the
// sound volume setting can be applied; frame() applies it (main thread,
// between the game's frames) once FMOD is up and whenever it changes.
void install();
void frame();

// "sound: mixing 6.2 ms per 42.7 ms of sound, 0 dropouts, 171 ms queued" for
// the log (mixing time since the previous call), or "" before the sound starts.
std::string summary();

}  // namespace game::audio
