# Changelog

All notable changes to the port. The release zip's `README.txt` carries the same list.

## [0.3.1]

### Fixed
- **After a long session, cars stopped appearing in the garage, and then the next race stayed on
  a black screen.** Reported with a `log.txt` from about an hour of play. The game starts a new
  thread for every car the garage shows and for every loading screen. The Switch gives a game 96
  threads, and a thread gives its place back only when it is released. The Switch's homebrew
  library can't release a thread by itself (`pthread_detach` does nothing there), and the port
  relied on it, so every finished thread kept its place, and its 1 MiB of memory, until the game
  was closed. In the report, after 88 of the game's threads and the port's own 8, no new thread
  could start: the car being loaded never showed, and the next race never loaded. The port now
  releases each thread as soon as it has finished, so the count stays the same however long you
  play. Every version up to 0.3.0 had this; browsing many cars in the garage brought it on sooner.

### Changed
- The `perf:` lines in `log.txt` end with the number of threads the port has running
  (`threads 10`), so a report shows at once if threads ever pile up again.
- As after every update, the first start rebuilds the JIT cache.

## [0.3.0]

### Added
- **Dock and undock while you play.** Up to 0.2.8 the resolution was chosen when the game
  started: after docking or undocking, the console scaled the picture, and only a restart gave
  the native resolution. The game itself can't change its screen size once it has started, so
  the port now changes the real screen around it: when the console is docked or undocked, it
  re-creates the window at the new size (1280×720 handheld, 1920×1080 docked) and draws
  everything the game renders at that size, the 3D scene included. A game started handheld and
  then docked renders a true 1080p; one started docked and then undocked renders 720p again.
  The switch takes a moment and is logged in `log.txt`. When you don't dock or undock, nothing
  changes.
- **The game's texts name the Switch's buttons.** The game was written for a MOGA gamepad, so
  its controls page and tutorials said "MOGA", "Start" and "Select". In all 11 languages they now
  say "Switch", + and − ("SWITCH GAME CONTROLS", "Tap the + button to PAUSE"). The port changes
  the game's text tables in memory as the game reads them, one text at a time (the game shares
  words such as "START" between texts; only the controller ones change); the OBB on your SD card
  is not touched. On the map, the EasyDrive button shows a + in its middle.
- **New controller pictures on the controls pages.** The drawing of the MOGA gamepad is replaced
  by two drawings of the port's own, in the game's style: a handheld console in handheld mode and
  a gamepad when docked; the picture changes by itself when you dock or undock. The buttons are
  where they are on the Switch: X on top, Y left, A right, B bottom, − and + at the top, L and R
  on the shoulders. The pages' callout lines follow: on the game controls page the drift line
  points at A and "Tap the + button to PAUSE" sits above the picture, pointing at +; on the menu
  controls page everything moves down to make room for EASYDRIVE above the picture, the face
  buttons' callouts show what each button does, and the callout for Origin (EA's online account,
  not available offline) is gone.

### Fixed
- **A rare crash of the game while loading into a race.** Found while testing 0.3.0 (it is in
  0.2.8 too): a function of the game that stores per-frame values could be called before the slot
  it writes to existed, and the game crashed reading address 0x3C. It is a sibling of the timing
  function fixed in the first release, with the same symptom. The function now stores nothing
  until its slots exist.

### Changed
- **An honest description of how the port works.** The README called the port "not a phone
  emulator" right before saying that the game's code is translated on the fly. Both were true,
  but together they were misleading: the CPU part is emulation (the game is 32-bit ARM code,
  Switch homebrew can only run 64-bit code, so Dynarmic recompiles it), and nothing else is
  (no phone, no Android system, no emulated GPU). The README, the player's guide and
  [How it works](docs/TECHNICAL.md#how-it-works) now say so plainly, and explain why the game's
  code can't run on the Switch without translation.
- **Performance: unchanged.** Nothing from earlier versions was taken out. On the PC test build,
  in the same automated race with the JIT cache, the race's loading thread took 3.2–3.6 s
  (0.2.8: 3.3–3.4 s) and the game's code 10.1–11.0 ms per frame (0.2.8: 10.4–10.7 ms, medians of
  each run): the same, within the runs' own spread. Docking support costs a comparison per frame
  until the console's mode changes; the texts are changed once, on a background thread at
  start-up, and the picture once per texture pack as the game loads it.
- As after every update, the first start rebuilds the JIT cache.

## [0.2.8]

### Fixed
- **The port crashed at start-up on some consoles**, right after loading the game's libraries
  (every version up to 0.2.7). The game expects its memory to start out as zeros, as on a phone.
  On the Switch, the memory the port takes still held what the program that ran before it left
  there (the Homebrew Menu, a forwarder), and the port did not clear it. Where those leftovers
  reached the game libraries' data, the game's C++ library started up with garbage and crashed.
  How much is left over depends on what ran before, so it happened on some consoles and not on
  others. The port now clears its memory before the game starts (a fraction of a second).
- **A cut-off or damaged game library made the port close right after it started, without a
  message.** The loader trusted the library's headers and read past the end of the file. It now
  checks each library as it loads it. One that is cut off or malformed is deleted, and the port
  says so: put your APK back into `game/` and start again, and the port takes a fresh copy out
  of it.
- The game libraries are read in one go instead of through a stream, a few hundred bytes at a
  time. In the Switch-like test build (the console's memory and JIT model, emulated on a PC,
  which exaggerates the difference), loading the 11 MB game library went from 1.3 s to 0.06 s.

### Added
- **Crash reports in `log.txt`.** When the port crashes, `log.txt` now ends with a `CRASH`
  section: what went wrong, where (an offset in the port's code that can be looked up in that
  release's `nfsmw.elf`), which game thread it was and what it was doing. Before, the log could
  stop up to a second before the crash, with nothing about it. An error the port did not expect
  (such as running out of memory) now ends with a message instead of closing silently.
- `log.txt` shows each library's size and CRC-32, and says when one is not the 1.3.128 build the
  port was tested with, or when one changed after the setup took it out of the APK (an SD card
  error, most likely). The setup keeps those checksums in `game/lib/checksums.txt`.
- `log.txt` starts with the console's firmware version, how the port was started (as an
  application with R + a game or a forwarder, or as an applet) and by which loader, and the
  memory it may use; after the start-up reservations it logs the memory left.

### Changed
- `log.txt` is written straight through the SD card's file system and closed whenever the port
  ends.
- As after every update, the first start rebuilds the JIT cache.

## [0.2.7]

### Faster loading
- **Translated code is reused.** The game's ARM code is translated for the Switch's CPU the
  first time it runs, and the game starts a new thread for every loading screen, which used to
  start with nothing translated. A new thread now takes over a finished thread's translations.
- **New: the JIT cache.** The port remembers which parts of the game's code were translated
  (in the new `cache/` folder) and, from the second start on, translates them ahead of time:
  for the loading screens while the game starts up, and for the races while the game waits
  for the graphics at the end of each frame. On the PC test build, with the cache from an
  earlier session, the garage's loading thread needs 0.6 s instead of 2.8 s and the race's
  3.6 s instead of 7.9 s (the console is slower, but gains in the same way). Turn it off
  with `jit_cache=off` in `settings.ini`; `cache/` can be deleted at any time.
- Decompressing the game data (zlib's `inflate_fast`) and hashing it now run as native code,
  bit-identical to the game's own code.

### Fewer stutters
- **0.2.6 read the game data one byte at a time through the C library** (the data cache had
  no buffer in front of it), and on the Switch every one of those bytes made the library
  check every open file. The data streams have a buffer again. This affected both the loading
  screens and the races.
- With the JIT cache, code a race needs for the first time (a police chase starting, the crash
  camera) is usually translated before it runs, instead of stopping the game for a moment.

### Added
- `log.txt` now shows how long each loading screen took (`thread ... finished after ...`),
  how much new code the main thread had to translate (`JIT:`), the JIT cache, the memory in
  use, the texture formats the game uses and any shader the graphics driver rejects.

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

[0.3.0]: ../../releases/tag/v0.3.0
[0.2.8]: ../../releases/tag/v0.2.8
[0.2.7]: ../../releases/tag/v0.2.7
[0.2.6]: ../../releases/tag/v0.2.6
[0.2.5]: ../../releases/tag/v0.2.5
