# Changelog

All notable changes to the port. The release zip's `README.txt` carries the same list.

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

[0.2.5]: ../../releases/tag/v0.2.5
