# Changelog

All notable changes to the port. The release zip's `README.txt` carries the same list.

## [0.2.6]

### Fixed
- **Short stutters while the counter shows 30 FPS.** Three causes, all on the game's main thread:
  - The game reads its data file (the OBB) in thousands of small pieces, and each one was a
    separate request to the SD card. The OBB is now read through a 128 MiB memory cache in
    128 KiB blocks, with a background thread reading ahead of the game when it loads something
    large. In a desktop test run (start-up and a race), the game's threads went from over 12,000
    small reads from the card to about 150 block reads; the rest was read ahead in the background.
  - Every line written to `log.txt` went to the SD card at once, from the thread that wrote it.
    The log is now written in the background, at least once a second.
  - The `perf:` line in `log.txt` now counts these stutters ("hitches": frames that take 1.5x
    longer than they should) and shows the OBB cache and the sound.
- **Sound breaking up.** The sound is mixed by the game's own code (FMOD), and that mixing shared
  a CPU core with the graphics thread, which had priority: when graphics were busy the sound
  had to wait and broke up. It now runs on another core, before the game's other threads.
- **Crackling at loud moments.** The game mixes so loud that engines, crashes and music go past
  the loudest sample the sound output can play (up to 1.8 times in a test race) and are cut off.
  The new **sound volume** setting (debug menu, default **70%**) lowers the mix before that
  happens. Turn the console's volume up instead.

### Changed
- Cleaner sound: converted from the game's 24 kHz to the Switch's 48 kHz with a proper filter
  instead of straight-line interpolation, and about half the delay (171 ms instead of 341 ms).
  If the sound ever drops out, the port keeps a little more sound queued from then on.

## [0.2.5]

### Added
- **Easy install, no PC needed.** Copy your APK and OBB (or one `.xapk` file) into the game folder
  and start the port. At the first start it takes the libraries and assets out of the APK by
  itself, with a progress screen, then starts the game.
- Usual copying mistakes are fixed automatically: libraries left in `lib/armeabi-v7a/` or loose in
  `game/`, the assets folder in the wrong place, the OBB in a subfolder or renamed.
- A check for APKs that only have the 64-bit build, with a clear message.

### Changed
- Clearer messages when a game file is missing: they say to put the APK into the game folder.
- A new icon in the Homebrew Menu.

## 0.2.0

### Changed
- **New name: NFS Most Wanted 2012**, in `switch/NFSMostWanted2012/`, so it isn't confused with the
  port of Most Wanted (2005). Game files and progress are moved from the old
  `switch/NFSMostWanted/` folder automatically, and the old version is removed.
- `save/` is split in two: `save/progress/` holds your career (the game's one save file) and
  `save/settings/` the game's own settings. Saves of 0.1.0 are moved there automatically.
- The CPU and memory information the game reads is built into the `.nro` (no more
  `save/system/` folder).
- Fewer text files: `README.txt` (with the changelog) and `LICENSES.txt`. Old text files are
  removed from the folder at the first start.
- The source code ships in `source/`, inside the port's folder.
- The `.nro` file name carries its version, and `log.txt` starts with it. Older `.nro` files of
  the port in the same folder are removed at start-up (never newer ones).

## 0.1.0

First release.

- The Android game 1.3.128 (armeabi-v7a) runs on the Switch, fully offline, at 720p handheld and
  1080p docked, with fixes for two crashes in the game's own code.
- Controller support everywhere, in the game's own style, plus optional gyro steering; the touch
  screen keeps working.
- Debug menu (ZL + ZR + Minus) with performance settings and a frame-rate counter.
- Performance work: graphics calls on their own CPU core with redundant ones dropped, faster memory
  access and atomic operations, native reading of the game data, Medium detail and stock clocks by
  default.
- One tidy folder (`game/` and `save/`), set up by `prepare_sd.py`, which can also give the port
  the game's own icon.

[0.2.6]: ../../releases/tag/v0.2.6
[0.2.5]: ../../releases/tag/v0.2.5
