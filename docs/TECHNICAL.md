# How the port works

[← Back to the README](../README.md)

Technical notes for people who want to know what happens inside, or who want to work on the port.
Building it is in [BUILDING.md](BUILDING.md).

- [How it works](#how-it-works)
- [What has been tested](#what-has-been-tested)
- [Performance](#performance)
- [Threads](#threads-031)
- [Big blocks at the top of the heap](#big-blocks-at-the-top-of-the-heap-032)
- [Java objects: freeing what the game no longer uses](#java-objects-freeing-what-the-game-no-longer-uses-032)
- [Text in every language: the console's fonts](#text-in-every-language-the-consoles-fonts-032)
- [Vibration](#vibration-032)
- [The game's icon on the player's copy](#the-games-icon-on-the-players-copy-032)
- [Online services are blocked](#online-services-are-blocked)
- [Resolution](#resolution)
- [The Switch's buttons in the game's texts](#the-switchs-buttons-in-the-games-texts-030)
- [Controller support in the menus](#controller-support-in-the-menus)
- [Gyro steering](#gyro-steering)
- [Debug menu (performance settings)](#debug-menu-performance-settings)
- [The port's folder and the first-start setup](#the-ports-folder-and-the-first-start-setup)
- [Source layout](#source-layout)

## How it works

The Android game is a 32-bit ARM program; Switch homebrew runs 64-bit code only. The loader:

1. **Runs the 32-bit code through a CPU recompiler.** [Dynarmic](https://github.com/azahar-emu/dynarmic)
   (the recompiler used by 3DS emulators) translates each block of the game's ARM code into
   host code the first time it runs, and from then on the host CPU runs the translation. This
   is the one part of the port that is emulation, of the CPU only (see below).
2. **Loads the game's `.so` files itself** (`src/core/elf.cpp`) into a 32-bit guest address
   space and links them against host implementations of everything they import.
3. **Re-implements the Android system libraries the game uses** (`src/hle/`): about 330 libc,
   math and pthread functions, plus OpenGL ES 2.0, the Android log and the bitmap API.
   Calls go guest → `svc` stub → C++ function, with soft-float ARM calling-convention
   marshalling (`src/core/abi.h`).
4. **Fakes the Java side** (`src/hle/jni/`): a JNI environment with on-demand classes. The
   game's Java glue (`GameActivityMain`, `RunLoop`, `AssetManager`, MOGA controller, FMOD's
   audio device, `Paint`/`Bitmap` text rendering) is re-implemented in C++.
5. **Drives the Android lifecycle** (`src/main.cpp`) the way `GameActivityMain` does: create,
   start, resume, surface created/changed, then one run-loop tick per frame.

The Switch-specific parts are in `src/platform/platform_switch.cpp` (EGL/Mesa, HID, touch,
audout, memory) and a small Dynarmic patch (`patches/dynarmic-switch.patch`). The patch makes
the JIT write code through one mapping and run it from another, as Horizon requires.

**Why the game's code is translated rather than run as it is.** The Switch's Cortex-A57 can
execute 32-bit ARM (A32/T32) code, but Horizon runs a thread in 32-bit state only in a process it
created as a 32-bit one; a 64-bit process cannot switch to 32-bit code for part of its work.
Horizon does support 32-bit processes (some commercial games are 32-bit), but homebrew is 64-bit
throughout: an `.nro` runs inside the
Homebrew Menu's 64-bit loader process, and libnx, devkitA64 and the Mesa/nouveau GPU driver the
port is built on exist only for AArch64. Running the game's code without translation would mean
a 32-bit Switch runtime, graphics driver and C library that do not exist, and a port installed as
a title instead of started from the Homebrew Menu. So the CPU is the one emulated part:
Dynarmic, its JIT cache (`src/core/jit_cache.cpp`) and native versions of a few hot game
functions (`src/game/native.cpp`) keep its cost down. Nothing else is emulated: there is no
Android system image, no Dalvik/ART virtual machine and no emulated GPU. The game's OpenGL ES
calls go to the Switch's GPU driver (through a thin layer that tracks state and scales the
screen), and its file, sound, input and Java calls are answered by the port's own code.

## What has been tested

Tested on the desktop build with the original 1.3.128 APK and OBB:

| Part | State |
|---|---|
| Loading + linking the game's 5 libraries | working |
| Start-up: EA / Firemonkeys logos, title screen, loading | working |
| EA User Agreement screen, text rendering (fonts from the APK) | working |
| Touch input (tapping CONFIRM) | working |
| First race: 3D world, cars, HUD, AI opponents | working |
| Steering with the analog stick (game sees a MOGA Pro gamepad) | working |
| Music and sound effects (FMOD) | working (recorded on desktop) |
| Save data (`files/var/nfstr_save.sb`) | working |
| Online services (EA servers, telemetry) | blocked; the game runs fully offline |
| Playing every menu with the controller (touch still works) | working |
| Gyro steering (optional, buttons unchanged) | working on desktop with simulated tilt; not yet tried on a console |
| Debug menu with performance settings (ZL + ZR + Minus) | working on desktop; the clock and VSync settings only act on a console and are untested there |
| GL calls on their own thread, redundant GL calls dropped | working on desktop (rendering checked against the unthreaded path) |
| Direct guest-memory access in the Switch JIT | working in the ARM64 build under qemu; not yet tried on a console |
| Docking / undocking while playing: the resolution follows (0.3.0) | working on desktop with simulated mode changes, both ways; not yet tried on a console |
| Switch button names in the texts (11 languages), the controller pictures and their callout lines (0.3.0) | working on desktop on both controls pages, with every texture pack size the Switch uses (720p and 1080p starts), and with the picture changing while the page is open |
| Random crash at start-up (a bug in the game's texture loading) | fixed in the loader (`src/game/fixes.cpp`) |
| Rare crash loading into a race (a timing function reaching a missing object) | guarded in the loader (`src/game/fixes.cpp`) |
| Rare crash loading into a race (the per-frame value store reaching a missing slot, 0.3.0) | guarded in the loader (`src/game/fixes.cpp`); the guard checked by calling the function with a slot taken away (`NFSMW_TEST_SLOT_NULL=1`) |
| Threads released when they end (0.3.1) | working on desktop with the Switch's limit simulated (`NFSMW_THREAD_LIMIT`): the old behaviour left the garage on its spinner after a few cars, the new one loaded 40 cars in a row and a race with 10–11 threads running |
| Phone effects, anti-aliasing, anisotropic (texture) filtering (0.3.1) | working on desktop (flags read back, effects and cost measured in an automated race; the anti-aliased and filtered pictures checked); not yet tried on a console |
| CPU boost while loading (0.3.1) | the detection of loads checked on desktop (which has no clocks to change); not yet tried on a console |
| Debug menu pages, temperature and battery (0.3.1) | the pages checked on desktop; the temperatures and battery are read only on a console, not yet tried there |
| Vibration from crashes (0.3.2) | working on desktop: crashes matched to the game's light / medium / hard shake and turned into rumble values (logged with `NFSMW_RUMBLE_LOG`); the HD rumble itself only on a console, not yet tried there |
| Vibration from nitro (0.3.2) | the player's nitro gauge found and read on desktop; using nitro could not be tried there (the tutorial blocked it), not yet tried on a console |
| Russian, Japanese, Korean and Chinese text (0.3.2) | working on desktop with Noto CJK in place of the console's fonts (the language list and menus checked in Japanese and Russian); the console's own fonts not yet tried |
| The game's icon and title on the `.nro` (0.3.2) | working on desktop on a copy of the released `.nro` (icon, title in all 16 languages, RomFS intact); on the console not yet tried |
| Freeing unused Java objects (0.3.2) | working on desktop, also with a pass after every 150 new objects (`NFSMW_GC_OBJECTS=150`) through the map, events and the language menu |
| Big blocks at the top of the heap (0.3.2) | working on desktop at 1080p with the console's 1 GiB heap; the docked map on a console after several events not yet confirmed |
| 720p handheld / 1080p docked | working (1080p rendering checked on desktop) |
| First-start setup from the APK or `.xapk`, fixing misplaced files | working on desktop; the Switch's setup screen and its restart are not yet confirmed on many consoles |
| Same start-up on ARM64 with the Switch memory/JIT model (qemu) | working |
| Switch build (`NFSMostWanted2012-<version>.nro`) | runs on the console (Atmosphère, emuMMC); built with devkitA64 (GCC 16.1, libnx, switch-mesa 20.1). The first test build froze the console once (cause not known) |

## Performance

The goal is a steady 30 fps at the Switch's normal (stock) clocks. Measured with a virtual
clock, the game moves by the real time between frames but never by more than about 1/30 s:
at 60 fps it still runs at normal speed, below 30 fps it runs in slow motion. So holding 30
is what matters.

Where a race frame's time went on the game's main thread, before the work below (desktop
test machine, detail level Medium, `cmd profile`): about 11 ms of translated game code (about
2 million ARM instructions) plus about 11 ms in host calls, almost all of it inside the GL
driver for roughly 4,000–5,000 GL calls. What the loader does about it:

- **GL on its own thread** (`src/hle/gl_queue.cpp`). The game makes all of its GL calls from
  its main thread, in between its own code. Each call is now only written into a queue
  (with a copy of any data it points to) and a separate GL thread, which owns the context,
  runs them. On the Switch that thread gets core 2 to itself (the game's threads use core 1,
  its main thread core 0). The buffer swap and its wait for the screen refresh also happen on
  the GL thread, overlapping the next frame's game code; at most one frame is queued ahead.
  In a race the game makes only calls that return nothing, so its thread never waits for the
  GL thread except at that swap. The loader's own overlay is drawn on the GL thread too.
  Main-thread time spent on GL calls: about 11 ms per frame before, under 1 ms after.
- **Redundant GL calls dropped** (`src/hle/gles.cpp`). The game re-binds textures and buffers
  that are already bound, re-sends unchanged uniform values and switches its vertex arrays off
  after every draw and on again before the next. Those calls are filtered out before they reach
  the queue: about 1,650 per frame, leaving roughly 1,400–1,700 instead of 4,000–5,000.
  Screenshots with and without the filter are identical apart from the game's animations.
- **Direct memory access in the JIT** (Switch). Horizon has no user-mode fault handling, so
  the translated code went through a page-table lookup for every load and store. It now adds
  the guest-memory base directly, as on the desktop; a small Dynarmic patch allows this without
  a fault handler. Worth about 5 % of the game code's time (desktop and qemu measurements).
- **Atomic operations** (Switch). The game's code does about 4,600 atomic updates (LDREX/STREX)
  per frame on its main thread in a race. On the Switch each one goes through Dynarmic's global
  exclusive monitor, which scanned the reservation of all 256 possible guest threads on every
  store; it now scans only the handful in use (Dynarmic patch). With the Switch's memory model
  on the desktop, the main thread's time in the monitor went from about 9 % to under 0.5 %.
- **OBB reading in native code** (`src/game/native.cpp`). In a race the game streams the
  world from the OBB, a zip archive with encrypted entries, on its main thread: every byte was
  decrypted, copied and CRC-checked by translated ARM code, a byte at a time (about a tenth of
  the main thread's time while streaming, in short bursts that showed as stutter). The
  decryption loop, the copy loop and `crc32` now run as native code (the Switch's CPU has
  CRC-32 instructions), with bit-identical results.
- **Floating-point status flags not tracked** (Switch). After every block of translated code
  that does floating point, Dynarmic's ARM64 backend cleared and read back the CPU's
  floating-point status register, to keep the guest's cumulative exception flags. None of
  the game's libraries ever reads those flags (no `VMRS rX, FPSCR` in their code), so a small
  Dynarmic option now skips that (`NFSMW_FP_FLAGS=1` restores it for comparisons).
- **Medium detail level** by default (see the debug menu notes): about 18 % less game code
  and a quarter fewer GL calls than Highest.
- **OBB read cache** (`src/hle/file_cache.cpp`, 0.2.6). The game reads the OBB through stdio in
  small pieces: a few bytes to 12 KiB at a time, each after a seek, from its main thread and two
  streaming threads. On the Switch each read was a request to the SD card (newlib's `FILE`
  buffer is only 1 KiB), so a burst of them was a frame that took too long while the average
  still said 30 fps. The OBB is now opened as a `FILE*` (`fopencookie`) that reads from a
  shared cache of 128 KiB blocks (128 MiB, least recently used blocks dropped). A stream that
  reads forward from block to block gets the following blocks read by a background thread,
  1 block ahead at first and up to 8 (1 MiB) as it keeps going. In a desktop test run (start-up
  and a race): 12,491 reads and 17,005 seeks on the card before; 869 block reads after, 724 of
  them by the read-ahead thread, so the game's own threads read only 145 blocks themselves.
  `NFSMW_NO_OBB_CACHE=1` turns it off for comparisons, `NFSMW_OBB_TRACE=1` logs every block
  read.
- **Log written in the background** (`src/core/log.cpp`, 0.2.6). Every line of `log.txt` was
  written and flushed to the SD card by the thread that logged it, usually the game's main
  thread (the game prints bursts of warnings when its HUD animates). Lines now collect in memory
  and a background thread writes them at least once a second; errors, `fatal()` and the exit
  paths flush at once. Since 0.2.8 the file is written without the C library's `FILE` (on the
  Switch straight through the SD card's file system), so that a crash report can still add to
  it (see [Crash reports](#crash-reports-and-library-checks-028)).

What remains is the game's own code, translated: about 3 million ARM instructions per frame
in a race (Low detail saves only about 5 % of that; it mostly saves GL calls).

### Loading (0.2.7)

The game starts a new thread for every loading screen. Sampling one (desktop, `perf` with
Dynarmic's JIT map) put its time in four places: translating the game's code for that new
thread (about a third: each guest thread had its own, empty JIT), zlib's `inflate_fast`
(about a sixth), the C library's stdio refills (a sixth) and an FNV-1 hash over loaded data
(a twelfth). What changed:

- **Buffered OBB streams** (`src/hle/file_cache.cpp`). 0.2.6 opened the cached OBB streams
  unbuffered, and the game reads much of its data a byte at a time: every byte went through
  the C library's refill, which for an unbuffered stream first flushes every open output
  stream (the Switch's newlib walks the whole list of open files each time). The streams now
  have an 8 KiB buffer in front of the cache.
- **JIT reuse** (`src/core/cpu.cpp`). A finished guest thread keeps its JIT, with everything it
  translated, in a pool; the next new guest thread takes it over (the one with the most code)
  instead of starting empty. The translated code stays valid: the game's code does not change.
- **JIT cache** (`src/core/jit_cache.cpp`). Every block that gets translated is listed (its
  location descriptor: address and mode bits), per kind of thread, in `cache/jit-main.bin` and
  `cache/jit-workers.bin` in the port's folder; the lists are written in the background. In
  the next session a background thread translates the loading threads' list into a spare
  JIT (64 MiB of code cache on the Switch) while the game starts up, and that JIT goes to
  the first loading thread the game starts after its first frames (only the game's own
  threads, not the sound library's). The main thread translates its list while it waits for
  the GL thread at the end of a frame, so the code that a race needs the first time (a police
  chase starting, the crash camera...) is usually ready before it runs. The lists are tied
  to a fingerprint of the game libraries and the port version, and only addresses inside the
  game's libraries are kept. `jit_cache=off` in `settings.ini` (or `NFSMW_NO_JIT_CACHE=1`)
  turns it off; deleting `cache/` starts the lists again.
- **Native `inflate_fast` and FNV-1** (`src/game/native.cpp`): the same algorithms as the
  game's (zlib 1.2.8's `inffast.c`, on the game's own `z_stream` and `inflate_state`),
  verified against the game's code over thousands of calls with `NFSMW_VERIFY_NATIVE=1`
  (which runs both and compares everything they write).

Desktop measurements, startup and the first race of the test save (wall time of the game's
loading threads): the garage load went from 2.8 s (0.2.6) to 1.4 s, 0.6 s with the JIT
cache from an earlier session; the race load from 7.9 s to 5.3 s, 3.6 s with the cache. With
the cache the main thread translated about 20 new blocks when the race started instead of
18,000. The console's own numbers are in the `log.txt` lines `thread ... finished after ...`
(each loading thread's time, CPU time and translation time) and in the `perf:` lines
(`JIT: ... new blocks` on the main thread, `JIT cache: ...`).
`log.txt` gets a summary every 10 seconds (frame rate, slowest frame, hitches, game time per
frame, GL thread load, resolution, detail level, clocks, new code translated on the main
thread, the JIT cache, the OBB cache, the sound and the memory in use), so a console's
numbers come back with the log:
`perf: 27.3 fps (frame 36.6 ms, slowest 52 ms, 2 hitches) | game 31.2 ms/frame | GL thread 64% busy,
waited 2.1 ms/frame, swap 4.0 ms/frame | ... | game code (CPU, core 0): a higher CPU clock helps |
OBB cache: 108 MiB, 2704 block lookups (95% in memory), 869 card reads (108.6 MiB, 724 read ahead,
1157 ms), game waited 494 ms | sound: mixing 1.0 ms per 42.7 ms of sound, 0 dropouts, 171 ms queued`.
The part after the clocks names what holds the frame rate back: the game's code (main
thread), the GL driver (GL thread), or the GPU (time spent in the buffer swap waiting for
it). A hitch is a frame that took more than 1.5 times as long as it should (50 ms at 30 fps):
what an average frame rate hides. "Game waited" is the time the game's threads spent waiting
for the SD card since the start.
The defaults therefore leave all clocks at stock. If the frame-rate counter shows the game
below 30 fps on a console, the debug menu can raise the CPU clock (1224 MHz is a clock some
Switch games use themselves). These numbers come from the desktop build; real hardware will
tell how much headroom is left.

### Loading (0.3.1)

- **CPU boost while loading** (`loading_boost=on`, debug menu PERFORMANCE). A race's loading
  thread on the console spends most of its time running game code (`log.txt`: `thread ...
  finished after 38.6 s: CPU 32.8 s`), so the CPU clock sets its speed. While the game loads,
  the port raises the CPU to 1785 MHz, as official Switch games do for their loading screens,
  and puts the chosen clock back afterwards (`src/game/perf.cpp`). "Loading" is a loading screen
  (the game logs `Lock: Is being locked. Lock count: 1` when one starts and `Lock count: 0` when
  it ends) or a game thread started after the game began running that is working, not waiting
  for work (the game starts one per car shown in the garage and per race; its first thread
  waits in `pthread_cond_wait` for the whole session and does not count). The boost stays on
  for 1.5 s after a load, so browsing cars does not switch the clock for each car, and never
  lasts more than 120 s in one go. `log.txt` gets `loading screen: 24.1 s (CPU boost)` lines,
  and the debug menu's INFORMATION page lists the latest loads.
- **The JIT cache survives updates.** Up to 0.3.0 its lists were tied to the port version, so
  the first session after every update translated everything again (the first race load took
  several seconds longer). The lists only say where blocks of the game's own code start;
  translating one of them ahead is correct whatever the port version, because it translates
  the code as it is in memory. They are now tied to the game's code and a JIT generation number
  (changed only when Dynarmic or its patch changes).
- **The tiniest C library functions as guest code** (`src/hle/guest_fast.cpp`). Every C
  library function the game calls is a host function: the translated code leaves the JIT, the
  call is dispatched to C++, and the JIT is entered again. A race load calls `strcmp` about 3.5
  million times and `__aeabi_memclr8` 2.7 million times, `tolower` 680,000 and `strlen` 510,000
  times, each on a few bytes, so the round trip costs more than the work. These now run as small
  ARM routines in guest memory, translated like the game's own code (`__aeabi_memclr4/8` hand
  clears above 256 bytes to the host's `memset`). On the desktop build the race's loading thread
  went from 2.9 to 2.5 s of CPU time (`NFSMW_NO_GUEST_FAST=1` turns them off for comparison); on
  the console, where each host call costs more, the gain should be larger.
- **Where a load's time goes, on the console.** Each game thread that loads times its host
  calls, and its `thread ... finished after` line in `log.txt` ends with the total and the
  slowest ones (`host calls 4.10 s: fread 1.92 s x31000, ...`). The desktop profile of a race
  load is not the console's, and this shows what to work on next.

## Threads (0.3.1)

Each game thread runs on a host thread of its own (`src/hle/libc_pthread.cpp`), and the game
starts one for every car the garage shows and every loading screen. The Switch allows an
application 96 threads, counted until each thread is released, and a thread is released only
by joining it: libnx implements no `pthread_detach` (newlib's returns `ENOSYS` and keeps the
thread). Up to 0.3.0 the port detached the game's detached threads, so each one kept its
kernel thread and its 1 MiB stack after it ended, and after about 90 car and track loads
`pthread_create` failed (the car stayed missing, the next race never loaded). Now
`platform::detach_thread` keeps finished-or-running detached threads in a list and the next
`spawn_thread` or `detach_thread` joins the ones that have finished (`src/platform/thread.cpp`;
the same code on the desktop). Every game thread's host thread goes on that list as soon as it
starts; the game's `pthread_join` waits for the thread's record to be marked done instead of
joining the host thread. The `perf:` lines end with the number of threads not yet released.

## Big blocks at the top of the heap (0.3.2)

[Issue #1](../../../issues/1): docked, after two or three events the map stopped loading its
pictures, and entering an event then crashed. The report's `log.txt` could not be read here, so
this is what the PC test build shows at 1920x1080 with the console's 1 GiB guest heap
(`NFSMW_GUEST_MIB=1280`):

- The game picks its UI texture packs by screen size. At 640x360 it loads `published.1x`
  (the map pack is 4.2 MiB, the menu pack 28.6 MiB); docked it loads `published.4x`, and reads
  each pack whole into its heap: 58.0 MiB for the map, 66.7 MiB for the menu, 21.0 MiB for the
  race HUD, two or three such blocks alive at once. Loading the map takes the heap from about
  25 to about 250 MiB in use for a moment.
- The port's allocator (`src/core/memory.cpp`) used to give every block the smallest gap it fitted
  in, wherever that was. The big blocks landed between the game's long-lived data, and when they
  were freed the gaps they left were split by what had been placed around them: with the same
  47 MiB in use on the map each time, the largest free block went from 953 MiB to 883 MiB after
  one map → event → map round, with peaks down to 682 MiB while the packs loaded. A full event
  (results, garage, unlocks) keeps more data around than the quick rounds of the test.
- Android's own malloc gives blocks this big their own memory mapping, so the game never had this
  problem on a phone.

Since 0.3.2, blocks of 1 MiB and more are taken from the top of the heap (the highest free region
that holds them) and everything else from the bottom, so the big ones only ever sit next to each
other and their space joins up again when they are freed. `NFSMW_HEAP_BEST_FIT=1` restores the
old placement for comparisons.

`log.txt` shows the rest: each `perf:` line has `game heap X MiB in use, Y of Z MiB taken,
largest free W MiB`, every block of 16 MiB or more gets a `guest heap: 58.0 MiB block ...` line
with the largest free block left, and a failed allocation logs the request with what was in use
and free. If the map still stops loading, those lines say whether the game's heap ran out (and
how), or whether the cause lies elsewhere (the `memory X of Y MiB` figure covers the console's
memory as a whole, `GL textures` the game's textures).

## Java objects: freeing what the game no longer uses (0.3.2)

There is no Java VM in the port: the game's JNI calls reach a small table of fake classes
(`src/hle/jni/`), and up to 0.3.1 nothing in it was ever freed. `DeleteLocalRef` and
`DeleteGlobalRef` did nothing, so every object the game made stayed, and arrays and bitmaps
kept their block of the game's own heap. The game draws every piece of text (map labels,
event cards, menus) into a new `BitmapGraphics` bitmap, so the heap kept a little more after
every screen. It was the first suspect for [issue #1](../../../issues/1) (the map stops
loading after a few events, docked), but measured on the PC test build it is slow: about 1 MiB
of bitmaps in ten minutes at 360p, 4 MiB at 1080p. The cause found for issue #1 is described
under [Big blocks at the top of the heap](#big-blocks-at-the-top-of-the-heap-032); this leak is
fixed as well.

Objects now live as JNI defines it (`src/hle/jni/jni.cpp`):

- A **local reference** is recorded for the thread that receives it (a Java method's result,
  `NewObject`, `NewStringUTF`, `GetObjectField`, `GetObjectArrayElement`, ...). It ends with
  `DeleteLocalRef`, with `PopLocalFrame`, or when the native method that got it returns (each
  `jni::call_native` is a frame). Threads the game starts itself keep theirs until
  `DetachCurrentThread` or the end of the thread.
- A **global reference** (`NewGlobalRef`, and weak ones, kept strong) counts until
  `DeleteGlobalRef`.
- Once per frame, between frames, when more than 4000 objects or 8 MiB of arrays and bitmaps
  were made since the last time (at most once a second), a **mark-and-sweep pass** frees what
  nothing reaches any more. Roots: global and local references, static fields, classes, the
  objects made while the game started up and the ones the port's C++ code holds (`jni::pin`),
  and everything made since the pass before (C++ code may hold a new object for a moment
  before handing it over). Fields and object-array elements are followed whenever they look
  like a reference. A freed object's handle is not reused for 32 passes.

`log.txt` gets a `JNI: freed ...` line when a pass frees more than 1 MiB, and every 30
passes, with the objects alive and the game heap in use. `NFSMW_NO_JNI_GC=1` turns it off
(for comparisons).

## Text in every language: the console's fonts (0.3.2)

The game draws its texts through Android (`com.ea.ironmonkey.BitmapGraphics` with `Paint`,
`src/hle/jni/text.cpp`, rasterized with stb_truetype) in its own fonts, Gotham, which only
have Latin letters. On a phone, Android draws every character the font lacks with the phone's
own fonts; here those characters were missing, so the language menu showed nothing for
Русский, 日本語, 한국어 and 中文 ([issue #2](../../../issues/2)), and the game in one of those
languages had blank texts. Now each character the game's font does not have is drawn with the
console's own fonts, the shared fonts every Switch game can use (`plGetSharedFontByType`:
Standard, Korean, Chinese Simplified and Traditional), so no font is shipped with the port. For a
text with kana the Japanese font draws the kanji, for one with Hangul the Korean font, and for
Chinese text the Simplified or Traditional font that has every character. Line heights stay the
game font's. The desktop build uses Noto CJK and Droid Sans Fallback when installed
(`NFSMW_FALLBACK_FONTS="file[#index]=tag,..."` chooses others).

## Vibration (0.3.2)

The Android game never vibrates, so the port takes two things the game already does and turns
them into HD rumble (`src/game/rumble.cpp`; `platform::set_rumble` on the Switch sends one
`HidVibrationValue` per frame to the controller in use: handheld, Pro Controller, Joy-Con pair
or a single Joy-Con).

- **Crashes.** The game shakes the race camera on each of the player's crashes. Its collision
  code (libapp.so +0xC014C) compares the impact with three thresholds and sends the camera the
  Hard, Medium or Light collision shake from its tuning (active time, two shake speeds, two
  shake lengths; the tuning names are in the binary: `HardCollisionCameraShakeActiveTime`...).
  The port hooks the camera's `CameraShakes::Start` (+0x91DF0) and `Stop` (+0x91EB4) with a
  small entry hook (the jump to a trampoline that tells the port and then runs the game's two
  instructions it replaced). For a crash shake it finds which of the three tuning entries it
  is (the tuning object comes from the getter at +0x341FB8) and gives a thump of 0.35, 0.65 or
  1.0, mostly on the low-frequency motor, fading out over 0.2 to 0.6 seconds.
- **Nitro.** Starting nitro also sends a camera shake, but that one was not seen arriving on
  Android, so the port watches the player's nitro gauge instead: the HUD's nitro bar (Update at
  +0x1E11F0, hooked the same way) keeps the player's nitro component at +0xF4 (with the
  shared_ptr count block at +0xFC, checked so a finished race's component is never read), and
  its level is the float at +0xBC. While the level goes down, the player is using nitro: a
  steady buzz, mostly on the high-frequency motor, that stops 0.25 s after the level stops
  going down (so a pause stops it too).

The setting (`vibration=off|low|normal|strong`, debug menu CONTROLS & SOUND) scales it all by
0, 0.5, 0.75 or 1; changing it in the menu gives a short test pulse. The controller is told to
stop when the game goes to the HOME menu and while the debug menu is open. Every hook checks
the game's instruction words first and is left out, with a warning in `log.txt`, if they are
not the expected ones. `NFSMW_RUMBLE_LOG=1` logs each shake, the crash level it matched, the
nitro gauge and every change sent to the controller.

## The game's icon on the player's copy (0.3.2)

The released `.nro` has the port's own plain icon: no EA artwork is shipped. At start-up the
port takes the game's launcher icon out of the player's APK in `game/` (the adaptive icon's
two layers, the visible middle 72/108 of each side, else the plain icon), keeps it as
`game/icon.jpg` so it survives deleting the APK and updating the port, and writes it into the
`.nro` that is running, with the title "NFS Most Wanted 2012" in every language of its NACP
(`src/game/icon.cpp`). The `.nro` is rewritten through a temporary file with its RomFS
unmounted for that moment. The Homebrew Menu shows the game's icon from then on, and a
forwarder installed afterwards gets it too ([issue #3](../../../issues/3)); one installed
before keeps the old icon until it is installed again. `tools/prepare_sd.py --nro` gives the
same result on a computer. `NFSMW_NO_ICON=1` leaves the `.nro` alone.

## Crash reports and library checks (0.2.8)

**The crash itself.** The tester's Atmosphère crash report (0.2.7) showed a data abort on the
main thread inside translated code, during `elf::run_initializers`, at the host address of
guest address `0xFF322D2D`; the guest return addresses in Dynarmic's return stack buffer were
libc++'s `ios_base::Init::Init` → `basic_streambuf()` → `locale::locale()`, with a pointer into
libc++'s `.bss`. The ELF loader copied each segment's file part and left the rest (`.bss`) as
the guest memory already was, and on the Switch the guest space is heap memory that the
Homebrew loader hands over from the program that ran before in the same session (the
Homebrew Menu, a forwarder): with leftovers there, libc++'s static locale looked initialised
and pointed at garbage. `NFSMW_TEST_DIRTY_MEMORY=<MiB>` (desktop) fills the guest space with
leftovers and reproduces the crash at the same place. Since 0.2.8, `mem::init` clears the
guest space when the platform says it may hold leftovers (the Switch: all of it, about 1.25
GiB, logged as `guest memory cleared`) and `elf::load` zero-fills each segment's `.bss`.

A tester's `log.txt` from 0.2.7 ended right after `loaded libNimble.so`, with no error: the port
had closed while loading or just after loading the game's main library, `libapp.so`. Two things
made that log useless, and both changed:

- **The log lost its last second.** Lines are written in the background (see above), so a crash
  took the lines of the last second with it, and nothing recorded the crash itself. Now a
  crash adds a report to the log (`src/core/crash.cpp`): the lines not written yet, then a
  `CRASH` section with what went wrong, where (the crash address as an offset in the port's
  code, which `addr2line -f -e` resolves with that release's symbols,
  `NFSMostWanted2012-<version>-symbols.elf` on the release page), the caller, the faulting address, which game thread crashed and what it was
  doing (inside one of the port's own versions of an Android function, or in translated game
  code, with the guest registers), the general registers and the stack words that point into
  the port's code. On the Switch, libnx calls `__libnx_exception_handler`
  (`src/platform/platform_switch.cpp`) on the crashed thread, on a 32 KiB stack of its own;
  on Linux a signal handler does the same and leaves faults inside the JIT to Dynarmic's own
  handler (fastmem). The report allocates no memory and takes no lock that could be held: it
  writes through the same file handle as the log. When the handler returns, libnx ends the
  program and the system shows its error (Atmosphère writes its own report too). A C++
  exception nobody caught (`std::bad_alloc` when memory runs out) is reported the same way and
  ends with a message. `NFSMW_TEST_CRASH=<libs|init|frame>:<host|guest|thread|throw|oom>` crashes
  on purpose, to check all this.
- **A damaged library crashed the loader.** `elf::load` read the program headers and segments
  where the file's headers said, without checking that the file was that long: a cut-off copy
  made it read past the end of its buffer (on the desktop test build, a `libapp.so` cut to
  1 or 4 MB crashed it right after `loaded libNimble.so`, without a message). Each library is
  now checked: headers, segments, the dynamic section and the tables it points to must lie
  inside the file, and relocations inside the library. One that is not is deleted, with a
  message saying to put the APK back so that the setup takes a fresh copy. The libraries are
  also read in one go (0.2.7 read them through a stream into a vector, a few hundred bytes per
  request to the SD card).

Before any of that, `log.txt` gets a `system:` line on the Switch (firmware version, whether it
runs on Atmosphère, the applet type the port was started as and the loader's name, the memory
free of the total) and, once the guest memory and the main thread's JIT are reserved, the
memory left: the facts that differ between consoles where the same build works and fails.

`log.txt` also gets each library's size and CRC-32. The setup writes the size and CRC-32 of
every library it takes out of the APK to `game/lib/checksums.txt`; at each start, a library
that is neither the 1.3.128 build the port was tested with nor what the setup wrote is
reported as changed (an SD card error, most likely), and one from another APK build is noted.
Neither stops the game: modified APKs have libraries of their own.

## Online services are blocked

The loader never lets the game reach EA's servers. Everything the game uses to go online is
refused in `src/hle/net_block.cpp`. That covers sockets (`socket`, `connect`, `send`, ...) and
name lookups (`getaddrinfo`, `gethostbyname`). On top of that:

- the Switch build does not start the console's network service at all;
- the game is told the device has no network (EA Nimble `Network.getStatus()` returns `NONE`);
- telemetry is switched off before the game starts (`files/enable.telemetry` = `0`);
- requests to open web pages or the store are ignored.

Career, races, cars and saves all work offline. Features that need EA's servers (Autolog,
online leaderboards, EA account, in-game store and update checks) are unavailable. The log
shows each blocked function the first time the game tries it
(`online services blocked: socket() refused`) and, at exit, how many calls were refused.

## Resolution

| Console mode | Renders at |
|---|---|
| Handheld (or tabletop) | 1280 × 720 |
| Docked | 1920 × 1080 |

The game's interface scales with the screen, so menus and HUD keep the same proportions at
both sizes.

**Docking and undocking while playing (0.3.0).** The game reads its screen size only once, at
start-up, and cannot change it: after a full GL context re-creation at a new size it keeps
drawing its old view. Up to 0.2.8 the port therefore kept the start-up resolution and let the
console scale the picture to the new screen. Since 0.3.0 the port changes the real screen
instead and leaves the game's idea of it alone:

- The main loop checks the console's mode once per frame. When it changes, the GL thread
  re-creates the window surface at the new size (`platform::gl_resize`: the EGL surface is
  destroyed, the window's buffers are released and resized, a new surface is made; the GL
  context, and so every texture and shader, stays). If that fails, the old size is kept and
  the console scales the picture, as before.
- The GL layer gets an output factor (`hle::gl_set_output_size`): the new screen size divided
  by the game's start-up screen size (×1.5 when a game started handheld is docked, ×0.667 the
  other way round). Viewports, scissor boxes and copies on the screen are multiplied by it, and
  the game's screen-sized render targets are re-created at their new size (the 3D resolution
  setting multiplies on top). Blits between the screen and those targets stay consistent
  because both sides scale together; a read-back with mismatched factors is logged once.
- The port's own drawing (the menu highlight, the debug menu, the frame-rate counter) uses the
  same factor, with text rendered at the new pixel size.

So a game started in handheld mode and then docked renders a true 1080p, and one started
docked and then undocked renders its 3D scene at 720p again, as a handheld start does. When
the console's mode does not change, the cost is one comparison per frame and one per viewport
call. The resize itself takes a few frames; `log.txt` logs it with its duration
(`display: now 1920x1080 ...`). On the desktop test build, `NFSMW_TEST_DOCK` simulates the
mode changes (for example `600:1920x1080,1800:1280x720`).

### Phone effects (0.3.1)

The game picks a detail level from the phone's speed (`getPerformanceScore()`; the port reports
a score that gives the level chosen in the debug menu) and turns it into a word of feature flags,
once: libapp.so's function at `0x3A4E5C` fills a static struct at `0xAD6518` (guarded by the byte
at `0xAD6528`; `0x3A54E8` returns it), and code all over the game tests single bits of it. The
words the game sets (read back on the desktop build, `log.txt`: `game features: ...`):

| Detail level | Flags |
|---|---|
| Low | `0x50406820` |
| Medium | `0x409070E0` |
| High | `0x00BF57E2` |
| Highest | `0x0EBFD7E2` |

High sets nine flags Medium does not (1, 8, 9, 10, 16, 17, 18, 19, 21) and clears two that
Medium sets (13 and 30). Named from the code that tests them: 8 turns on the environment's fog,
18 the road's specular and wet-road effect (and car particle effects), 19 the engine's
reverb/low-pass filter, 21 a part of the car shader's lighting, 10 the sky reflection textures.
On the desktop build, in the same automated race (`NFSMW_FEATURES_ADD`, see BUILDING.md), each
of the nine alone changed neither the game code's time per frame nor the GL calls measurably,
and all nine together on Medium cost 12.1 ms per frame against 12.0 ms (2,670 GL calls against
2,570), while High itself took 16.2 ms (3,150 calls), as did Medium with High's exact word. So
what makes High slow is clearing 13 and 30: Medium's flags for drawing fewer objects.

`phone_effects=high` (the default) adds High's nine flags to Medium's word before the game
first reads it (on the first frame the port calls the game's own getter, then changes the
word; `src/game/fixes.cpp`). `highest` also adds 15, 25 and 26, which only Highest sets
(brake lights, a live hemisphere map for the car reflections): that cost 15.0 ms per frame, as
much as High. The option acts at detail level Medium (`highest` also at High) and takes effect
at the next start.

### Anisotropic filtering and anti-aliasing (0.3.1)

Two graphics options the game itself does not have on Android, in the debug menu's GRAPHICS page
and in `settings.ini`; both change while the game runs (`src/hle/gles.cpp`):

- **Anisotropic filtering** (`anisotropic_filtering=off|2x|4x|8x|16x`, default 4x; 0.3.1 called
  it "Texture filtering", `texture_filtering=`, and a `settings.ini` with that line is still
  read and rewritten with the new name). The game never asks
  for anisotropic filtering, so textures seen at a slant (the road ahead, building walls) blur.
  Every texture the game gives a mipmap minification filter gets
  `GL_TEXTURE_MAX_ANISOTROPY_EXT` at the chosen level (clamped to what the GPU allows; Maxwell
  allows 16x); a change of the setting is applied to all of them at once. It costs the GPU a
  little texture bandwidth and nothing on the CPU.
- **Anti-aliasing** (`anti_aliasing=off|2x|4x`, default off). The game has multisampling code
  (for its off-screen buffer, behind a feature flag only Highest sets and a GL extension check),
  but the GL calls it would use are empty functions in the Android build, so no phone ever got
  it. The port does it below the game: while it is on, the game's framebuffer 0 is a
  multisampled framebuffer (RGBA8 + depth/stencil renderbuffers at the real screen size).
  After the game's frame, before the port's own overlays, it is resolved into a single-sample
  RGBA8 framebuffer and copied to the window (two blits, so the window may have any format).
  Reads from framebuffer 0 (`glReadPixels`, `glCopyTexSubImage2D`) resolve first and read the
  window; `glGetIntegerv(GL_FRAMEBUFFER_BINDING)` reports the game's own numbering. Docking or
  undocking re-creates the buffers at the new size. At detail level Highest the game draws the
  race scene into a texture of its own first, so the setting smooths only the menus there.
  Its cost is GPU time and memory bandwidth, more at 1080p.

## The Switch's buttons in the game's texts (0.3.0)

The game was made for touch screens and the MOGA gamepad, and the port hands it a MOGA Pro (see
`src/hle/jni/`). Its controls page (pause > CONTROLS, or *Options > Controls*) and its tutorials
therefore said "MOGA", "Start" and "Select", next to a drawing of the MOGA. Since 0.3.0 they show
the Switch's buttons:

- **Texts** (`src/game/obb_patch.cpp`). The game measures and draws its text one glyph at a time
  through the Java `BitmapGraphics` class, so the texts have to change at their source: the string
  tables `published/stringdata/<language>/nfsmw_android.sb` in the OBB (EA's chunked SBIN format:
  a string table in `CHDR`/`CDAT`, and `StringPair` records in `DATA` that tie each key to its
  text). At start-up a background thread reads the 11 tables (one per language) and changes the
  texts (`UI_MOGA_START` and `UI_MOGA_SELECT` become + and −, "Start" in the pause tutorial becomes
  +, "MOGA" becomes "Switch", or "SWITCH" in titles in capitals; on the menu controls page
  `MOGA_CONTROLS_SHOP` / `_MOST_WANTED` and `_BACK` / `_CONFIRM` trade texts, see below: 187 in
  all). Texts are shared between keys ("START" is also the race's `HUD_INGAME_START`, "CONFIRM"
  the age gate's button), so a shared text is never edited: each new text is added to the table
  and only that key's `StringPair` points at it. Then the chunks' FNV-1 checksums are recomputed
  (chunks start at multiples of 4 bytes). The OBB
  on the SD card is never written to. The changed tables go into an in-memory tail that the OBB
  cache (`src/hle/file_cache.cpp`, `set_tail`) shows the game after the real end of the file:
  the tables, stored uncompressed, then a copy of the zip's central directory that points at
  them, and a new end record. The game opens the OBB as a zip, finds the new directory at the
  new end and reads the new tables; everything else still comes from the card. About 1.2 MiB
  of memory with the layouts below, a few tens of milliseconds on a background thread, before the
  game opens the OBB.
- **The pictures** (`src/game/controller_art.cpp`). The drawing on those pages,
  `hud/MogaController.tif`, sits in RGBA texture atlases in the in-game and menu texture packs
  (`published.{1x,2x,4x}/texturepacks_ui/{in_game,menu}.sba`, the same 628 × 431 picture in all
  six; the menu packs store premultiplied alpha). The packs' `Box` records give each picture's
  place in its atlas; the atlases are stored bottom-up. When the game uploads one of those
  atlases (`glTexImage2D`), the port checks the atlas's size and a checksum of parts of the
  MOGA drawing, and if they match, writes one of its own two drawings in its place: a handheld
  console in handheld mode, a gamepad when docked. The port remembers those textures (and
  forgets them when the game deletes them); when the console is docked or undocked, it uploads
  the other drawing into them (`glTexSubImage2D`). The drawings are original, made from code in
  the game's colours by `tools/make_controller_art.py` and stored as PNG files in
  `src/game/controller_art_images.inc`. They have the Switch's button arrangement (X top, Y
  left, A right, B bottom, − and + at the top), with each element where a callout line points.
  The game draws the picture squashed to 0.866 of its height on a 16:9 screen, so the drawings
  are made in that space and stretched back.
- **The callout lines** (`published/layouts/layouts.sb`, the same SBIN format: `Entity` records
  with a rectangle in layout units around the page's centre, a rotation and a content such as a
  `Text` or a `SubLayout`; each line is a sublayout, `options_help_guide_left`/`_left2`/`_right`/
  `_right2`, made of a stretched straight piece and a fixed 40 × 20 bend whose end is a corner of
  the rectangle). The port changes 24 entries, checking every old value first:
  on the game page the drift line ends on A, and the pause callout moves above the picture with a
  line down to +; on the menu page everything moves 20 units down (as on the game page), the
  EASYDRIVE callout moves above the picture with a line down to +, the Origin callout (EA's
  online account) moves off the page, and the face buttons' lines keep their places while their
  texts trade (the line to the top button now says what X does, and so on). On the map, the
  EasyDrive button's label (now "+" instead of "START") moves to the middle of the round button,
  in the larger font.

Both can be turned off for comparisons: `NFSMW_NO_OBB_PATCH=1` (texts and lines) and
`NFSMW_NO_ART_PATCH=1` (pictures). `NFSMW_OBB_PATCH_DUMP=1` logs every text changed.

## Controller support in the menus

The menus were made for touch, so the loader watches which buttons the game
draws each frame and where (`src/game/ui_nav.cpp`). It marks the highlighted one in the
game's own style (`src/game/overlay.cpp`): a thin frame in the game's accent red just outside
the button, bright corner brackets like the game's own selection markers, a soft red glow
and, a moment after it arrives and then every few seconds, a slanted light sweeping across
the button at the angle of the game's slanted buttons. Nothing is drawn over the button
itself, so slanted buttons keep their shape. The highlight glides from button to button,
breathes slowly, closes in and flashes when A is pressed; A "taps" it. The overlay's
geometry goes to the GPU in one or two draw calls per frame. A popup is handled before the screen behind it. After a
press, the button that appears in the same place on the next screen ("continue", "OK") is
highlighted automatically. Buttons that need EA's servers are never picked first. The car
grid, driver profile and store already have the game's own controller support, with its own
highlight, so there the D-pad and A go straight to the game.

## Gyro steering

Tilt the console, or the Pro Controller / Joy-Cons, like a steering wheel. It is switched in the
game: **Options > Controls > TILT TO STEER** (also in the pause menu under **Controls**); a
"GYRO: ON / OFF" label on that tile shows the current state. The left stick still steers and takes
over whenever you move it. Holding the console level drives straight; about 30 degrees of tilt is
full lock.

The choice is saved in `settings.ini` in the port's folder (together with the debug menu's settings), which also has two optional settings:
`gyro_max_angle` (degrees of tilt for full lock, 10-60; smaller is more sensitive) and
`gyro_invert` (set to `on` if the car turns the wrong way). The game's own tilt mode cannot
be used with a gamepad connected; it only asks you to disconnect it. That is why the loader
does the tilt steering itself (`src/game/motion.cpp`).

## Sound

FMOD Ex mixes the game's sound in the game's own (translated) code. On Android its Java helper
`org.fmod.FMODAudioDevice` calls `fmodProcess()` for each block of samples and writes it to an
`AudioTrack`; `src/game/audio.cpp` runs the same loop and writes to the platform's output.
FMOD mixes at 24 kHz, 1024 frames (42.7 ms) per call.

- **Thread** (0.2.6): on the Switch the loop runs on core 1 at a priority above the game's
  worker threads. In 0.2.5 it ran on core 2 below the GL thread, which has that core first,
  so whenever the GL thread was busy the mixing waited and the sound broke up. Mixing takes
  about 1 ms per 42.7 ms block on the desktop test machine; the `sound:` part of the `perf:`
  line gives the console's figure.
- **Output** (`src/platform/audio_switch.cpp`, 0.2.6): each block is converted to 48 kHz and
  queued as one 2048-frame `audout` buffer. The queue starts 4 buffers deep (171 ms: how far
  the sound runs behind the game; 0.2.5 kept 4 buffers of twice the size, 341 ms). If the
  output ever runs dry (a dropout: the mixer was late) the queue keeps one buffer more from
  then on, up to 8. Dropouts are counted in the log; a pause of the mixer (the game stopping
  its sound) is not counted.
- **Conversion** (`src/platform/resampler.h`, 0.2.6): a 32-tap windowed-sinc (Kaiser)
  interpolator instead of straight-line interpolation, whose mirror images of the high
  frequencies came out only 12–20 dB down (now below −90 dB).
- **Sound volume** (0.2.6): the game's mix is loud enough to go past the 16-bit limit, where
  FMOD cuts it off: in a recorded test race about 0.4 % of all samples, up to 2.8 % in the
  loudest ten seconds, with peaks 1.8 times full scale. The loader hooks
  `FMOD_System_Create` to learn the FMOD system and sets the volume of its master channel
  group (applied on the main thread between frames, so never in the middle of the game's
  own FMOD calls). At the default 70 % the same race clips in 0.001 % of samples. The value is
  `sound_volume` in `settings.ini` and in the debug menu; `NFSMW_SOUND_VOLUME` overrides it
  for tests.

## Debug menu (performance settings)

Hold **ZL + ZR** and press **Minus** (the order does not matter) to open the port's own menu.
The same combination, **B** or **+** closes it. Opening it during a race pauses the race first
(with the game's own pause), and while it is open the game gets no input. ZL and ZR do nothing
in this game, so the combination never triggers a game action.

Since 0.3.1 it has four pages, switched with **L / R** or by tapping their tabs. **Up/Down**
picks a setting, **Left/Right** or **A** changes it. The touch screen works too: tap a row, tap
the left or right half of a value to step it down or up, tap outside to close. Each setting
shows a line of help, and a warning when the chosen value can make the game run worse. The
bottom line shows the live frame rate, frame time and GL calls, so the effect of a change is
visible right away. Changes are saved immediately to `settings.ini` in the port's folder.

| Page | Setting | Choices | Recommended (default) | What it does |
|---|---|---|---|---|
| PERFORMANCE | Frame rate limit | 30 / 60 / off | **30** | The game is made for 30 fps. 60 only helps if the game really reaches it. |
| PERFORMANCE | VSync | on / off | **on** | Off can tear. With 30 fps and VSync the display shows every second refresh (swap interval 2). |
| PERFORMANCE | CPU clock | stock (1020) / 1224 / 1428 / 1581 / 1785 MHz | **stock** | The translated game code runs on the CPU. Raise it only if the counter stays below 30 fps. |
| PERFORMANCE | GPU clock | auto / stock / 307.2 – 768 MHz | **stock** | The game is light on the GPU. Auto = 460.8 MHz handheld (Nintendo's highest handheld clock), stock when docked. |
| PERFORMANCE | Memory clock | stock / 1331.2 / 1600 MHz | **stock** | 1600 MHz is the docked memory clock; Nintendo also uses it in handheld mode. |
| PERFORMANCE | CPU boost while loading | on / off | **on** | 1785 MHz while the game loads, then the clock above (see [Loading](#loading-031)). |
| PERFORMANCE | GL on its own core | on / off | **on** | The GL thread described under [Performance](#performance). Applies at the next start. |
| PERFORMANCE | Direct memory access | on / off | **on** | Switch only: the JIT accesses guest memory directly. Turn it off if the game crashes. Next start. |
| GRAPHICS | Detail level | highest / high / medium / low | **medium** | The game's own detail tier. Applies the next time the game starts. |
| GRAPHICS | Phone effects | off / high-end phones / top phones | **high-end phones** | High's effects at Medium (see [Phone effects](#phone-effects-031)). Next start. |
| GRAPHICS | Anti-aliasing | off / 2x / 4x | **off** | Multisampling of what the game draws to the screen. Costs GPU time. |
| GRAPHICS | Anisotropic filtering | off / 2x – 16x | **4x** | On every mipmapped texture: the road ahead stays sharp. Called "Texture filtering" (`texture_filtering=`) in 0.3.1; that line is still read. |
| GRAPHICS | 3D resolution | 100 % – 50 % | **100 %** | Draws the race scene at a lower resolution and stretches it back; menus and HUD stay sharp. Only acts at detail level Highest (see below). |
| GRAPHICS | Frame-rate counter | off / FPS / full | off | Top-left corner. Full adds frame times, GL calls, clocks and resolution. |
| CONTROLS & SOUND | Tilt steering | off / on | off | The gyro steering under [Gyro steering](#gyro-steering) (also in the game's Options > Controls). |
| CONTROLS & SOUND | Tilt for full lock | 15 – 60 degrees | **30** | Smaller = more sensitive. |
| CONTROLS & SOUND | Invert tilt | off / on | off | For a car that steers the wrong way. |
| CONTROLS & SOUND | Vibration | off / low / normal / strong | **normal** | Rumble for crashes and nitro (see [Vibration](#vibration-032)). A short pulse when changed. |
| CONTROLS & SOUND | Sound volume | 100 % – 50 % | **70 %** | Volume of the game's mix before it is turned into 16-bit samples (see [Sound](#sound)). Above 70 % loud moments crackle. |

INFORMATION shows live figures: the version and detail level, the screen and 3D resolution with
the graphics options in use, frame rate and times, the clocks the hardware runs at (and whether
the loading boost is on), the SoC and board temperatures (`ts`) and the battery (`psm`), the
game's memory, the threads running, the latest loading times and the session's length.

"Restore recommended settings" puts the PERFORMANCE and GRAPHICS settings and the sound volume
back to the defaults above (tilt steering stays as it is). Values that make the game slower are
allowed on purpose (that is the point of a test menu); the help line says so when one is picked.

Notes:

- **Clocks** are left at stock unless you change them. The choices only use rates that Nintendo's own performance profiles use (nothing above
  1785 MHz CPU / 768 MHz GPU / 1600 MHz memory). They are set through `clkrst` (`pcv` before
  firmware 8.0) and re-applied when the system resets them (dock/undock, sleep). The stock
  clocks are given back when the game goes to the HOME menu and when it is closed: the loader
  asks to be notified of both (focus mode "suspend with notification", and an exit lock only
  while custom clocks are in use), so closing from HOME first restores the clocks, then lets
  the system end the game. At stock clocks the exit is never locked, so the system can always
  close the port. If sys-clk
  is installed, it may override them.
- **Detail level**: the game chooses its tier once at start-up from a score the loader reports
  (`getPerformanceScore()`); left alone it would pick Highest on the Switch. Highest adds a
  full-screen speed-blur pass, for which the scene is drawn into an off-screen buffer; that
  buffer is what the 3D resolution setting shrinks. High looks like Highest without the blur
  and costs about the same CPU time. Medium and Low draw about a quarter fewer objects
  (3,500–5,100 GL calls per frame in a race instead of 5,300–6,700) and the game code needs
  about 18 % less CPU time (10.4 instead of 12.7 ms per frame on the desktop test machine).
  The difference on screen is small (mostly car reflections and the missing speed blur).
  If the counter shows the game holding 30 fps with room to spare, try High or Highest (and,
  at Highest, a lower 3D resolution if the GPU is the limit); if it stays below 30, try Low.
- The 3D resolution is applied live: the loader allocates the game's screen-size render targets
  at the smaller size and scales viewports while they are bound (`src/hle/gles.cpp`).

## The port's folder and the first-start setup

The port finds its files in the folder it was started from, so the folder and the `.nro`
can be renamed or moved. `settings.ini`, `log.txt` and `save/` are created at the first start.

The game keeps all of its career in one binary file (`nfstr_save.sb`, EA's chunked SBIN
format): money, reputation, cars and events are records inside it, so they cannot live in
separate files without rewriting the game's save code. `save/progress/` holds that file alone,
so backing up or restoring a career is one file. The CPU and memory information the game reads
from `/proc` and `/sys` is built into the `.nro` (RomFS, from `res/romfs/`).

Folders set up by earlier versions (`lib/`, `apk/`, `fs/`, `save/internal/`, `save/external/`,
`save/system/`, the OBB next to the `.nro`) are reorganized into this layout automatically the
first time the port starts, keeping the progress. The port's folder under its earlier names,
`sd:/switch/NFSMostWanted/` (0.1.0 to 0.2.0, before the rename) and `sd:/switch/nfsmw/` (test builds), is
emptied into the new folder the same way: the game files, saves and settings are moved (never
overwriting anything), then the old version's `.nro`, guides, log and source code are removed,
and the folder too if nothing else is left. That only happens when the old folder really holds
this port's files (the game's libraries, the OBB or the saves). An older `.nro` of the port next
to the running one is removed as well; a newer one never is.

### First-start setup

Players only copy their APK and OBB (or one `.xapk` / `.apks` bundle) into `game/`. When the
libraries, the assets or the OBB are missing, the port sets itself up before anything else
(`src/game/setup.cpp`):

1. It fixes the usual copying mistakes: the libraries left in `lib/armeabi-v7a/` or loose in
   `game/`, an extracted `assets` folder one level too deep or its contents loose in `game/`,
   the OBB in a subfolder (an unzipped bundle's `Android/obb/...`) or under another name.
2. What is still missing it takes out of the APK: the 5 `armeabi-v7a` libraries and the
   `assets/` folder. In a bundle it reads the APK inside (stored, or compressed and unpacked
   first) and copies the OBB out of it too. Each file is written through a `.part` file and
   checked against the archive's CRC-32; compressed entries are unpacked with stb_image's
   zlib decoder. The libraries' sizes and CRC-32s go into `game/lib/checksums.txt` (see
   [library checks](#crash-reports-and-library-checks-028)).
3. A text screen shows the progress. Then the port asks the homebrew loader to start it again
   (`envSetNextLoad`, with `--after-setup` so it never loops), so the game gets a clean screen.

It stops with a clear message for an APK that only has the 64-bit build, a damaged archive or
a full SD card. The APK is never changed or deleted; the setup screen says it can be deleted.

## Source layout

```
CMakeLists.txt  builds the Switch .nro (devkitPro) and the Linux test build
README.md, CHANGELOG.md, THIRD_PARTY.md, CONTRIBUTING.md, LICENSE
docs/           install guide, these notes, building; images/ for the README
.github/        issue and pull request templates
src/
  core/         guest memory + heap, ELF loader, Dynarmic CPU threads (and JIT reuse), JIT cache,
                host-call thunks, log, crash reports, CRC-32
  hle/          libc/libm/pthread/GLES2/Android APIs (GL thread: gl_queue; OBB cache: file_cache),
                fake JNI + Java classes
  game/         NFS-specific config, the port's folder layout (paths), first-start setup from the
                player's APK (setup), controls (MOGA), gyro
                steering (motion), controller menu navigation (ui_nav, overlay), debug menu
                (debug_menu), performance settings (perf, prefs = settings.ini), fixes for game
                bugs, native versions of hot game code (native), FMOD audio pump, the
                game's texts and controller picture with the Switch's buttons (obb_patch,
                controller_art)
  platform/     Linux (desktop) and Switch backends (Switch sound output: audio_switch,
                with the 24 to 48 kHz converter in resampler.h)
tools/
  prepare_sd.py     fills the port's game/ folder from a player's APK and OBB
  make_release.py   packs the port, prepare_sd.py and the source into one release zip
  gen_gles.py       GLES thunk generator;  profile.py  sampling-profile report
  make_controller_art.py  draws the controls pages' pictures (src/game/controller_art_images.inc)
  fetch_deps.sh     fetches the third-party sources in external/
res/
  icon.jpg                  the port's own Homebrew Menu icon (256x256)
  switch-README.txt         the player's guide (README.txt next to the .nro), with the changelog
  romfs/system/             CPU and memory files the game reads, built into the .nro
  licenses/                 license texts of the components devkitPro provides
cmake/          toolchain file for the ARM64 Linux test build
patches/        the Dynarmic changes, as a patch against upstream
external/       Dynarmic (with the patch applied) and stb
```
