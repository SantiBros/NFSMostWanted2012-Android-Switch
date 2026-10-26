# Building the port

[← Back to the README](../README.md)

- [Building for Switch](#building-for-switch)
- [Desktop test build (Linux)](#desktop-test-build-linux)
- [Making a release](#making-a-release)
- [Third-party code](#third-party-code)

## Building for Switch

Requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `switch-dev`,
`switch-mesa` and `switch-libdrm_nouveau` installed (`dkp-pacman -S switch-dev switch-mesa switch-libdrm_nouveau`),
CMake 3.16 or newer, and the Boost headers on the build machine (Dynarmic uses a few
header-only Boost libraries; devkitPro has no Boost package). Install them with
`apt install libboost-dev` (Debian/Ubuntu), `brew install boost` (macOS) or
`pacman -S mingw-w64-x86_64-boost` (MSYS2), or point CMake at them with
`-DNFSMW_BOOST_INCLUDE=<folder that contains boost/>`. Only the `boost/` folder is passed
to the Switch compiler.

```
cmake -B build-switch -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-switch -j
```

The result is `build-switch/NFSMostWanted2012-<version>.nro`. Copy it to `sd:/switch/NFSMostWanted2012/` (or
pack a release with `tools/make_release.py`).

## Desktop test build (Linux)

```
cmake -B build -G Ninja && cmake --build build
NFSMW_ROOT=<port folder> ./build/nfsmw --frames 300 --screenshot-every 30
```

`<port folder>` has the same layout as on the Switch (`game/` with the OBB, `lib/` and
`assets/`; `save/` is created), so a folder made by `prepare_sd.py` works for both.

It renders off-screen with Mesa at 1280x720 (`--width 1920 --height 1080` for the docked size)
and writes PNG screenshots. `NFSMW_WAV=out.wav` records the audio.
The frame-rate limit from `settings.ini` applies here too (30 by default); set
`fps_limit=off` there for fast automated runs, or pass `--fps N` for one run.
Debug helpers: `NFSMW_INPUT="2000 tap 590 535; 2500 stick -0.5 0 10; 2600 press A"`
scripts touch and controller input by frame. `NFSMW_TRACE_WINDOW=N` prints the host calls per N
frames. `NFSMW_DUMP_EVERY=N` prints what every game thread is waiting on. `NFSMW_TRACE_JNI=1` logs
every Java call the game makes.
`NFSMW_CONTROL=<dir>` drives the game interactively: commands appended to `<dir>/cmd.txt`
(`press A`, `press ZL+ZR+MINUS`, `tap x y`, `rstick 0 -1 20`, `tilt 30`, `wait 30`, `shot name`,
`cmd dumpui`, `cmd fbtrace`) run one after another. `tilt` simulates the motion sensor, in degrees;
`cmd dumpui` logs every button on screen with its name and position; `cmd fbtrace` logs the
framebuffers and viewports of the next two frames.
Profiling: `cmd profile` logs, for 200 frames of the main thread, the time and number of every
host call, the guest code time and the GL calls the filter dropped (`NFSMW_COUNT_INSNS=1`
adds the number of guest instructions). `cmd sample` takes a sampling profile of the
translated game code (run with `NFSMW_SAMPLE_OUT=<file>` and `PERF_BUILDID_DIR=<dir>`, then
`tools/profile.py <file> <dir>/perf-<pid>.map <log> <game lib dir>`). For comparisons:
`NFSMW_NO_GL_THREAD=1`, `NFSMW_NO_GL_FILTER=1`, `NFSMW_PAGE_TABLE=1` (page-table memory model)
and `NFSMW_UNSAFE_FP=1` (x86 only: skip exact ARM floating-point emulation).
`NFSMW_PATCH="libapp.so+0x340138=e3a03001,libapp.so+0x34013c=e5c43023"` turns on the game's
own debug AI driver, so races drive themselves during automated tests.
`NFSMW_TEST_DOCK="600:1920x1080,1800:1280x720"` changes the display size at those frames, as
docking and undocking do. `NFSMW_NO_OBB_PATCH=1` and `NFSMW_NO_ART_PATCH=1` leave the game's texts
and layouts, and its controls picture, as they are (MOGA); `NFSMW_OBB_PATCH_DUMP=1` logs every text
changed. For smooth recordings, `NFSMW_RECORD_FPS=30` gives the game a clock that advances 1/30 s
per frame whatever the PC's speed, and `NFSMW_SHOT_RANGE=<from>:<to>` limits `--screenshot-every`
to those frames (the race clip in the README: `--screenshot-every 1`). The controls pages' pictures are drawn by `tools/make_controller_art.py` (Pillow), which
writes `src/game/controller_art_images.inc`; `--preview DIR` also saves them as PNG files. `NFSMW_TEST_SLOT_NULL=1` checks the 0.3.0 game fix for
the per-frame value store: about 900 frames in, it calls that function with a slot taken away
(with `NFSMW_NO_SLOT_FIX=1` as well, the game crashes there, as it did before the fix).
`NFSMW_THREAD_LIMIT=96` refuses new threads past that number, as the Switch does, and
`NFSMW_NO_THREAD_REAP=1` never releases detached threads, as up to 0.3.0 on the Switch: together
they reproduce the 0.3.0 garage bug on the desktop (a car that never loads after enough loads).
`-DNFSMW_SWITCH_TEST=ON` builds with the Switch memory/JIT model. The ARM64 cross build
(`cmake/aarch64-linux.cmake`, `-DNFSMW_NO_GL=ON`) runs under `qemu-aarch64`.

## Making a release

Never commit or attach game files: not the APK, the OBB, the `.so` libraries, the extracted
assets, a `game/` or `save/` folder or a prepared `switch/` folder. The `.gitignore` keeps them
out of the repository. `python3 tools/make_release.py` packs everything into one zip for a
GitHub release, with no game files:

```
NFSMostWanted2012-<version>.zip
├── switch/NFSMostWanted2012/   goes on the SD card
│   ├── NFSMostWanted2012-<version>.nro   the port (plain icon)
│   ├── README.txt              the player's guide: the folder, install, controls, debug menu,
│   │                           troubleshooting, what's new, build version and SHA-256
│   ├── LICENSES.txt            the open-source components and their full licenses
│   ├── game/                   empty: the player's own game files go here
│   ├── save/progress/, save/settings/   empty: the game's saves go here
│   └── source/                 this repository's source code (not needed to play)
└── prepare_sd.py               run on a computer to fill game/
```

Next to the zip it writes `NFSMostWanted2012-<version>-symbols.elf`, the symbols of that exact
build (from `build-switch/nfsmw.elf`, without debug sections). Attach it to the release as well:
a crash report in `log.txt` gives addresses as offsets in the port's code (`nfsmw.elf+0x...`), and
`aarch64-none-elf-addr2line -f -C -e NFSMostWanted2012-<version>-symbols.elf 0x...` names the
function.

Only two text files go on the SD card. `README.txt` comes from `res/switch-README.txt` (with
the version, date and checksum filled in; its "What's new" section is the changelog), and
`LICENSES.txt` is generated from the component list in `tools/make_release.py` and the license
texts (`res/licenses/` for the components devkitPro provides). For each release, update
`NFSMW_VERSION` at the top of `CMakeLists.txt` (it sets the version shown in the Homebrew
Menu, the `.nro` and zip file names and the first line of `log.txt`) and add the version to
the "What's new" section.

Players copy `switch/` onto the root of the SD card and put their APK and OBB into `game/`; the
port sets itself up at the first start (`prepare_sd.py` is optional).

Then create a GitHub release with the tag `v<version>` and attach the zip and the symbols file.
The release notes (English, then Spanish) follow the same order every time:

1. One line on what the version brings, then the "No game files are included" box.
2. **"Which file do I download?" / "¿Qué archivo descargo?"**, a table: the zip is the only file
   players need; the `-symbols.elf` is for developers (crash reports); "Source code" is added by
   GitHub and has nothing to play with.
3. What's new (from [CHANGELOG.md](../CHANGELOG.md), in players' words), how to install, updating
   from the previous version, known issues.
4. At the end, the `.nro`'s SHA-256 (as `make_release.py` prints it).

## Third-party code

`external/` holds the third-party code the port builds with, so a checkout builds without
downloading anything: Dynarmic at a pinned commit with `patches/dynarmic-switch.patch` already
applied (and its own externals), and the stb headers. `tools/fetch_deps.sh` re-creates `external/`
from upstream if you want to start from the original sources. Licenses:
[THIRD_PARTY.md](../THIRD_PARTY.md).
