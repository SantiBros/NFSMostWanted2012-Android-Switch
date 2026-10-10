#pragma once

namespace game::audio {

// Start/stop the FMOD mixing thread (FMODAudioDevice.start()/stop() on Android).
void start();
void stop();

}  // namespace game::audio
