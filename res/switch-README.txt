===============================================================================
  NFS MOST WANTED 2012 FOR NINTENDO SWITCH               version @VERSION@
  A homebrew port by SantiBros
===============================================================================

WHAT THIS IS
------------
A port of the Android game Need for Speed: Most Wanted (version 1.3.128), the
mobile version of Most Wanted (2012), to the Nintendo Switch, as homebrew for
Atmosphere. The port runs the Android game's own 32-bit ARM code: it translates
that code for the Switch's CPU on the fly (with the Dynarmic recompiler),
provides the parts of Android the game expects (files, graphics through OpenGL
ES, sound, input) and drives it like a phone would.

The port contains NO game files. You need your own copy of the Android game
(its APK and OBB files); they stay on your SD card in the game folder.

This is not the Switch port of Need for Speed: Most Wanted (2005): that is a
different game and a different project. This one is called
"NFS Most Wanted 2012 Loader" in the Homebrew Menu and lives in
switch/NFSMostWanted2012/.

This project is not affiliated with or endorsed by Electronic Arts or Nintendo.


FEATURES
--------
  * 720p in handheld mode, 1080p when docked.
  * The whole game is playable with the controller: driving, and every menu
    with a highlight you move with the D-pad. The touch screen still works.
  * Optional gyro steering: tilt the console or controller like a wheel.
  * Debug menu (ZL + ZR + Minus) with performance settings, the sound volume
    and an FPS counter.
  * Online services are blocked: the game runs fully offline.
  * Your progress is kept in save/ and survives updates of the port.


THE FOLDER
----------
  switch/NFSMostWanted2012/
  +-- NFSMostWanted2012-@VERSION@.nro
  |                         the port, version @VERSION@
  +-- README.txt            this guide
  +-- LICENSES.txt          the open-source parts of the port and their licenses
  +-- game/                 YOUR game files (never share or upload them).
  |                         You put in two files, the port does the rest:
  |     main.1003128.com.ea.games.nfs13_row.obb
  |                         the OBB file, exactly as it is (do NOT unzip it)
  |     <your APK>.apk      the game's APK (or one .xapk file instead of both)
  |                         At the first start the port takes these out of it:
  |     lib/                the game's 5 libraries (lib/armeabi-v7a/ in the APK)
  |     assets/             the APK's assets/ folder
  +-- save/                 what the game saves (back it up from time to time):
  |     progress/           your career: nfstr_save.sb holds your money (SP),
  |                         reputation and Most Wanted rank, cars and upgrades,
  |                         and the events you have done. The game keeps all of
  |                         that in this one file.
  |     settings/           the game's own settings: language, game version,
  |                         the accepted user agreement
  +-- source/               the complete source code of the port (not needed to
  |                         play; safe to delete if you need the space)
  +-- settings.ini          the port's settings (created at the first start)
  +-- log.txt               what happened at the last start (written each start)

The folder and the .nro can be renamed or moved: the port always uses the
folder it was started from.


WHAT YOU NEED
-------------
  * A Nintendo Switch running Atmosphere, with the Homebrew Menu.
    Use emuMMC, and keep a backup of your SD card.
  * Your own copy of the Android game, version 1.3.128:
      - the APK, with the 32-bit build (lib/armeabi-v7a inside it)
      - the OBB file: main.1003128.com.ea.games.nfs13_row.obb (about 650 MB)
    or both together in one .xapk file.
  * About 1 GB free on the SD card.
  No computer program is needed: the console sets the game up by itself.


INSTALLING
----------
  1. Copy the "switch" folder from the release zip to the root of your SD card.
  2. Copy your APK and your OBB file into switch/NFSMostWanted2012/game/.
     Do not unzip or rename anything: the APK can have any name ending in
     .apk. If your copy of the game is one .xapk file, copy just that file.
  3. Start the port (see STARTING THE GAME). The first time, it sets the game
     up from your APK: a screen shows the progress (a few seconds, or about a
     minute for an .xapk), then press A and the game starts. After that, the
     APK (or .xapk) can be deleted from game/ to free space.

The port also fixes the usual copying mistakes by itself: the libraries left
in lib/armeabi-v7a/, an extracted assets folder in the wrong place, or the OBB
in a subfolder or with another name.

On a computer (optional): prepare_sd.py from the release zip does the same
before you put the card in the console, and can also give the port the game's
own icon from your APK (needs Python 3 and "pip install pillow"):

    python prepare_sd.py --apk NFS_MW.apk
           --obb main.1003128.com.ea.games.nfs13_row.obb
           --nro E:\switch\NFSMostWanted2012\NFSMostWanted2012-@VERSION@.nro
           --out E:\switch\NFSMostWanted2012
    (all on one line; E: is the SD card)


STARTING THE GAME
-----------------
Hold R while you start any game on the console. The Homebrew Menu opens with
the full memory of a game (title-takeover mode). Choose
"NFS Most Wanted 2012 Loader" (by SantiBros, version @VERSION@).

Do not start it from the Homebrew Menu in the Album: there, homebrew only gets
about 440 MB of memory, which is not enough for the game.


CONTROLS
--------
Driving (the game sees a MOGA Pro gamepad, which it supports natively):

  Left stick ................ steer
  A ......................... drift
  L, or right stick down .... brake / reverse
  R, or right stick up ...... nitro
  + ......................... pause

Menus:

  D-pad / left stick ........ move the highlight (at the edge of a list: scroll)
  A ......................... press the highlighted button
  B ......................... back
  L / R, then A ............. pick a page in the bottom menu bar
  Right stick ............... drag like a finger (scroll lists, pan the map)
  + ......................... EasyDrive (start the next event) on the map
  Touch screen .............. works as on a phone

Gyro steering: in the game, Options > Controls > TILT TO STEER (also in the
pause menu under Controls). Hold the console level to drive straight; about
30 degrees of tilt is full lock. The buttons work as above.

Debug menu: hold ZL + ZR and press Minus. Up/Down picks a setting, Left/Right
or A changes it, B closes it. Changes are saved at once in settings.ini.


UPDATING
--------
  1. Copy the "switch" folder of the new release onto the SD card, as when
     installing. game/, save/ and settings.ini are kept.
  2. Delete the .nro of the old version, so only one .nro is left in the
     folder (with two, the Homebrew Menu may start the old one). If you forget,
     the new version removes the old .nro itself the first time it starts.
     Your progress is not in the .nro: it stays in save/.
  3. Replace source/ with the new one too, if you keep it.

Coming from the folder switch/NFSMostWanted/ (the port's folder before it was
renamed in 0.2.0, to tell it apart from the Most Wanted (2005) port), or from a
test version in switch/nfsmw/: just copy the new "switch" folder onto the SD
card and start "NFS Most Wanted 2012 Loader". The first time it starts, it
moves your game files, progress and settings from the old folder into
switch/NFSMostWanted2012/ and removes the old version. You do not need to run
prepare_sd.py again.


TROUBLESHOOTING
---------------
  "Game files missing" or "Game data missing"
      The message names what is missing. Put your APK (or .xapk) and the OBB
      into switch/NFSMostWanted2012/game/ and start the port again.

  "Setting up the game did not work"
      The message says why: an APK with only the 64-bit build (the port
      needs the one with lib/armeabi-v7a), a damaged APK (copy it again), or
      not enough free space on the SD card.

  "... is not a valid OBB"
      The OBB must be the original file, unchanged. Do not unzip it.

  The game crashes or the screen stays black
      In settings.ini, try  direct_memory=off  and then  gl_thread=off
      (one at a time; each takes effect at the next start).

  Low frame rate
      Turn on the counter in the debug menu (Frame-rate counter: full).
      log.txt gets a "perf:" line every 10 seconds that ends by naming what
      holds the frame rate back (game code, graphics driver or GPU). Raising
      the CPU clock in the debug menu helps when it says "game code".

  Short stutters although the counter shows 30
      The "perf:" lines in log.txt count them ("hitches") and show how long
      the game waited for the SD card ("game waited"). Please send log.txt
      with a report. A slow or nearly full SD card makes them worse.

  The sound crackles
      Lower the Sound volume in the debug menu (70% is the default; 100% is
      the game's full mix, which crackles at loud moments). If the sound
      breaks up instead, log.txt counts the "dropouts": please send it.

  The console freezes
      Wait a moment and try to close the game from the HOME menu before you
      hold the power button: forcing the console off while it writes to the SD
      card can damage files.

When you report a problem, attach log.txt from this folder.


WHAT'S NEW
----------
0.2.6
  * Fewer short stutters while the counter shows 30 FPS. The game's data
    file (the OBB) is now read through a memory cache in big blocks instead
    of thousands of small reads from the SD card, with the next blocks read
    ahead in the background, and log.txt is written in the background.
  * The sound no longer breaks up when the graphics are busy: the game's
    sound mixing now has a CPU core where it comes first.
  * New setting: Sound volume (debug menu, default 70%). The game mixes so
    loud that loud moments are cut off and crackle; 70% leaves room for them.
    Turn the console's volume up instead.
  * Cleaner sound (a proper filter for the 24 to 48 kHz conversion) with
    half the delay.
  * The "perf:" line in log.txt now counts stutters ("hitches") and shows the
    data cache and the sound.

0.2.5
  * Easy install: copy your APK and OBB (or one .xapk file) into the game
    folder and start the port. At the first start it takes the libraries and
    assets out of the APK by itself, with a progress screen, then starts the
    game. No computer program needed any more.
  * Usual copying mistakes are fixed by themselves (libraries left in
    lib/armeabi-v7a/, the assets folder in the wrong place, a renamed OBB).
  * Clearer messages when something is missing, and a check for APKs that
    only have the 64-bit build.
  * A new icon in the Homebrew Menu.

0.2.0
  * save/ is now split in two: save/progress/ holds your career (money,
    reputation, cars, events: the game's one save file) and save/settings/
    the game's own settings. Saves of 0.1.0 are moved there automatically.
  * The CPU and memory information the game reads is built into the .nro:
    no more save/system/ folder.
  * Fewer text files: this README (with the changelog) and LICENSES.txt. The
    old text files are removed from your folder at the first start.
  * The source code is in source/, inside the port's folder.
  * New name: NFS Most Wanted 2012, in switch/NFSMostWanted2012/, so it is not
    confused with the port of Most Wanted (2005). Your game files and progress
    are moved from the old switch/NFSMostWanted/ folder automatically.
  * The .nro file name carries its version (NFSMostWanted2012-0.2.0.nro), and
    log.txt starts with the version.

0.1.0 (first release)
  * The Android game 1.3.128 (armeabi-v7a) runs on the Switch, fully offline,
    at 720p handheld and 1080p docked, with fixes for two crashes in the
    game's own code.
  * Controller support everywhere, in the game's own style, plus optional gyro
    steering; the touch screen keeps working.
  * Performance work: graphics calls on their own CPU core with redundant ones
    dropped, faster memory access and atomic operations, native reading of the
    game data, Medium detail and stock clocks by default.
  * One tidy folder (game/ and save/), set up by prepare_sd.py, which can also
    give the port the game's own icon.

Known issues: experimental; the very first test build froze a console once.
Performance at the console's stock clocks is still being tuned.


ABOUT THIS BUILD
----------------
  Version ........... @VERSION@
  Built ............. @DATE@
  NFSMostWanted2012-@VERSION@.nro, SHA-256 (as released):
      @SHA256@
      Giving the port the game's icon with prepare_sd.py --nro changes the
      file, and so this value.


CREDITS
-------
Port by SantiBros.

Built with Dynarmic, libnx, Mesa, libdrm_nouveau, newlib, fmt, mcl, oaknut,
robin-map, Boost and stb (see LICENSES.txt), and the devkitPro toolchain.

Need for Speed and Most Wanted are trademarks of Electronic Arts Inc. Nintendo
Switch is a trademark of Nintendo. This project is not affiliated with or
endorsed by either company. Please support the original game.
