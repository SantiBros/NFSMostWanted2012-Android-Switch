# Installing NFS Most Wanted 2012 on the Switch

[![English](https://img.shields.io/badge/lang-English-e11d2e?style=for-the-badge)](INSTALL.md)
[![Español](https://img.shields.io/badge/idioma-Espa%C3%B1ol-2b2f38?style=for-the-badge)](INSTALL.es.md)

[← Back to the README](../README.md)

> [!TIP]
> **New to Switch homebrew?** You first need a console that runs custom firmware (Atmosphère) with
> the Homebrew Menu. The community guide [switch.hacks.guide](https://switch.hacks.guide/) explains
> how, step by step. The words used on this page are explained in the [glossary](#glossary).

- [What you need](#what-you-need)
- [Your game files](#your-game-files)
  - [Getting the files from your own phone](#getting-the-files-from-your-own-phone)
- [Install](#install)
- [The first start](#the-first-start)
- [Starting the game](#starting-the-game)
- [The port's folder](#the-ports-folder)
- [Updating](#updating)
- [Saves and backups](#saves-and-backups)
- [Optional: the PC script](#optional-the-pc-script)
- [Troubleshooting](#troubleshooting)
- [Reporting a bug](#reporting-a-bug)
- [Glossary](#glossary)

## What you need

- A Nintendo Switch running [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) with the
  Homebrew Menu. Use **emuMMC** and keep a backup of your SD card.
- Your own copy of the Android game, version **1.3.128**.
- About **1 GB** free on the SD card.
- A computer only to copy files to the SD card (or any other way you use to copy files to it).

## Your game files

This project contains **no game files**. You take them from your own copy of *Need for Speed Most
Wanted* for Android, version **1.3.128** (package `com.ea.games.nfs13_row`):

> [!NOTE]
> Only the **Android** version works. The PC, PlayStation 3, Xbox 360, PS Vita, Wii U and iOS
> versions of Most Wanted (2012) are different programs, and their files can't be used.

| File | What it is |
|---|---|
| **APK**, any name ending in `.apk` | The Android app: the game's program code (5 libraries in `lib/armeabi-v7a/`) and small assets. It must contain the **32-bit** build, `lib/armeabi-v7a`; 64-bit-only APKs (`arm64-v8a`) don't work. |
| **OBB**, `main.1003128.com.ea.games.nfs13_row.obb` (about 650 MB) | The game data: cars, tracks, textures, sounds. It's a zip archive the game opens itself, so it must stay **exactly as it is**: don't unzip it. |
| or one **`.xapk`** (or `.apks`) | A bundle with both inside. Copy it as it is: the port reads the APK and the OBB out of it. |

### Getting the files from your own phone

If the game is installed on your Android phone, both files are already there:

- **The OBB**: on the phone's storage, in `Android/obb/com.ea.games.nfs13_row/`, named
  `main.1003128.com.ea.games.nfs13_row.obb`. Connect the phone to a computer with a USB cable, choose
  **File transfer** on the phone, and copy the file from that folder. (Recent Android versions hide
  `Android/obb` from apps on the phone itself, but a computer can usually still see it.)
- **The APK**: Android doesn't show it as a file, but an "APK extractor" app from the Play Store can
  save the APK of an installed app. Pick *Need for Speed Most Wanted* and copy the saved `.apk` to
  your computer. If it saves several files or an `.apks` file instead, copy that: the port reads
  bundles too.
- **Check the version**: in the phone's *Settings → Apps → Need for Speed Most Wanted*, the version
  must be **1.3.128**.

Developers can also use `adb`: `adb shell pm path com.ea.games.nfs13_row` shows where the APK is,
and `adb pull` copies it (and the OBB from `/sdcard/Android/obb/com.ea.games.nfs13_row/`).

## Install

<img src="images/install-steps.png" alt="Install in 4 steps: download the zip, copy the switch folder to the SD card, add your APK and OBB to the game folder, start with R + any game" width="100%">

1. On the [Releases](../../../releases) page, under **Assets**, download
   `NFSMostWanted2012-<version>.zip`. (Not "Source code (zip)": that is only the source code, for
   developers.) Unzip it on your computer.
2. Copy the `switch` folder inside it to the **root** of your SD card. (If your SD card already has
   a `switch` folder, the two merge; nothing else in it is touched.)
3. Copy your APK and your OBB into `switch/NFSMostWanted2012/game/`. Don't unzip or rename them.
   If your copy is a single `.xapk`, copy just that file.
4. Put the SD card back into the Switch and [start the game](#starting-the-game).

## The first start

When the game's libraries, assets or OBB are not in place yet, the port sets them up by itself
before the game starts. A text screen shows what it is doing:

1. **It fixes the usual copying mistakes**: libraries left in `lib/armeabi-v7a/` or loose in
   `game/`, an `assets` folder one level too deep (or its contents loose in `game/`), the OBB in a
   subfolder (an unzipped bundle's `Android/obb/...`) or under another name. Those are just moved
   where they belong.
2. **It takes what is still missing out of your APK**: the 5 libraries and the `assets` folder. From
   an `.xapk` it also copies the OBB out. Every file is checked against the archive's checksum.
3. When it's done, press **A**: the port starts again, straight into the game.

This takes a few seconds for an APK, or about a minute for an `.xapk` (copying the 650 MB OBB). It
happens only once. Afterwards the APK (or `.xapk`) is not needed any more, and you can delete it from
`game/` to free space. Your APK is never changed.

If something can't work, the port says why instead of starting: an APK with only the 64-bit build,
a damaged file, or not enough free space.

## Starting the game

Hold **R** while you launch **any game** on the HOME menu. The Homebrew Menu opens with the full
memory of a game (*title takeover*). Choose **NFS Most Wanted 2012 Loader** (by SantiBros).

> [!WARNING]
> Don't start it from the Homebrew Menu in the **Album**: there, homebrew only gets about 440 MB of
> memory, which is not enough for the game. The port shows a message if you try.

The game starts at **1280×720** in handheld mode and **1920×1080** when docked. It keeps the
resolution it started with: if you dock or undock while playing, the console scales the picture;
restart the game to get the native resolution for the new mode.

## The port's folder

```
sd:/switch/NFSMostWanted2012/
├── NFSMostWanted2012-<version>.nro   the port
├── README.txt                        the player's guide
├── LICENSES.txt                      open-source components and their licenses
├── game/                             YOUR game files (never share or upload them)
│   ├── main.1003128.com.ea.games.nfs13_row.obb   the OBB, unchanged
│   ├── <your APK>.apk                can be deleted after the first start
│   ├── lib/                          libapp.so, libc++_shared.so, libfmodex.so,
│   │                                 libfmodevent.so, libNimble.so
│   └── assets/                       the APK's assets
├── save/
│   ├── progress/                     your career: nfstr_save.sb
│   └── settings/                     the game's own settings: language, version, agreement
├── cache/                            the port's own cache (translated-code lists; safe to delete)
├── source/                           the port's source code (not needed to play)
├── settings.ini                      the port's settings (created at the first start)
└── log.txt                           what happened at the last start
```

The folder and the `.nro` can be renamed or moved: the port always uses the folder it was started
from. `source/` can be deleted if you need the space.

## Updating

1. Copy the `switch` folder of the new release onto the SD card, as when installing. `game/`,
   `save/` and `settings.ini` are kept.
2. The new version removes the `.nro` of older versions in its folder the first time it starts
   (never a newer one). You can also delete the old `.nro` yourself: with two `.nro` files in the
   folder, the Homebrew Menu shows both.
3. Replace `source/` with the new one too, if you keep it.

**Coming from 0.1.0 to 0.2.0** (folder `switch/NFSMostWanted/`) or from a test version
(`switch/nfsmw/`): just copy the new `switch` folder and start **NFS Most Wanted 2012 Loader**. The
first time, it moves your game files, progress and settings from the old folder into
`switch/NFSMostWanted2012/` and removes the old version. It only touches the old folder if it really
holds this port's files, and it never overwrites anything.

## Saves and backups

The game keeps your whole career in one file:

```
switch/NFSMostWanted2012/save/progress/nfstr_save.sb
```

Money (SP), reputation and Most Wanted rank, cars and upgrades and the events you've done are all
records inside it. Copy that file to back up your career, and copy it back to restore it. The game's
own settings (language, accepted agreement) are in `save/settings/`.

## Optional: the PC script

`prepare_sd.py` (in the release zip, needs Python 3) does the first-start setup on a computer
instead, before the SD card goes into the console. It can also give the port **the game's own
icon** from your APK, so the Homebrew Menu shows the Need for Speed icon (needs
`pip install pillow`). The released `.nro` never contains EA's artwork.

With the SD card as drive `E:` (all on one line):

```
python prepare_sd.py --apk NFS_MW.apk --obb main.1003128.com.ea.games.nfs13_row.obb --nro E:\switch\NFSMostWanted2012\NFSMostWanted2012-0.2.8.nro --out E:\switch\NFSMostWanted2012
```

macOS / Linux: `python3 prepare_sd.py ... --out /Volumes/SD/switch/NFSMostWanted2012`.

The script checks your files and says what it did: `libapp.so sha256 a73dcc17...: 1.3.128
(armeabi-v7a)` means your APK is the tested build.

## Troubleshooting

| Message or problem | What to do |
|---|---|
| **"Game files missing"** | Put your APK (or `.xapk`) into `switch/NFSMostWanted2012/game/` and start again: the port takes what it needs out of it. |
| **"Game data missing"** | The OBB is not in `game/`. Copy `main.1003128.com.ea.games.nfs13_row.obb` there, unchanged (or put your `.xapk` there). |
| **"... is not a valid OBB"** | The OBB must be the original file. Don't unzip, convert or re-zip it. |
| **"Setting up the game did not work"** | The message says why: an APK with only the 64-bit build (use one with `lib/armeabi-v7a`), a damaged file (copy it again), or not enough free space. |
| **"Please start the Homebrew Menu in title-takeover mode"** | Hold **R** while launching a game, then open the port. See [Starting the game](#starting-the-game). |
| **"A game file is damaged"** | One of the game's libraries in `game/lib/` was cut off or damaged (an interrupted copy, or an SD card error). The port has already deleted it: put your APK back into `game/` and start again, and it takes a fresh copy out of it. If this keeps happening, check your SD card on a computer. |
| **The game closes right after it starts** | Look at the end of `log.txt`: since 0.2.8 a `CRASH` section, or a line marked `E/`, says what happened. Make sure you started in title-takeover mode (**R + a game**). Then please [report](#reporting-a-bug) it with `log.txt`. |
| **The game crashes or the screen stays black** | In `settings.ini`, try `direct_memory=off`, then `gl_thread=off` (one at a time; each takes effect at the next start). |
| **Low frame rate** | Open the debug menu (ZL + ZR + Minus) and set *Frame-rate counter: full*. `log.txt` gets a `perf:` line every 10 seconds that ends by naming what holds the frame rate back (game code, graphics driver or GPU). Raising the CPU clock helps when it says "game code". |
| **Short stutters although the counter shows 30** | The `perf:` lines in `log.txt` count them ("hitches") and show how long the game waited for the SD card ("game waited"). Please [report](#reporting-a-bug) them with `log.txt`. A slow or nearly full SD card makes them worse. |
| **Loading is slow** | The first start after installing (or after deleting `cache/`) is the slowest: from the second start on, the port translates the game's code ahead of time (the JIT cache, in `cache/`). `log.txt` has a line `thread ... finished after ...` for each loading screen; please include it in a report. |
| **The sound crackles** | Lower *Sound volume* in the debug menu (ZL + ZR + Minus). 70% is the default; 100% is the game's full mix, which is cut off at loud moments. If the sound breaks up instead, `log.txt` counts the "dropouts": please report it. |
| **The console freezes** | Wait a moment and try to close the game from the HOME menu before you hold the power button: forcing the console off while it writes to the SD card can damage files. |
| **The Homebrew Menu shows the port twice** | An older `.nro` is still in the folder: delete it (or start the new version once, which removes it). |

## Reporting a bug

Open an [issue](../../../issues/new/choose) and attach **`log.txt`** from the port's folder. Please
include the port version, your firmware and Atmosphère versions, and whether you played handheld or
docked. If the game closed with an error, also attach the newest file in
`sd:/atmosphere/crash_reports/`, if there is one. Never attach game files (APK, OBB or `.so` files).

## Glossary

| Word | What it means |
|---|---|
| **APK** | The file an Android app is installed from. It holds the game's program code and small files. |
| **OBB** | The big data file of an Android game (cars, tracks, sounds). Here: `main.1003128.com.ea.games.nfs13_row.obb`. |
| **XAPK / APKS** | A bundle that packs the APK (and sometimes the OBB) into one file. The port reads them as they are. |
| **Atmosphère** | The custom firmware that lets a Switch run homebrew. |
| **emuMMC** | A copy of the Switch's system on the SD card, used to run custom firmware without touching the console's own system. The safer way to run homebrew. |
| **Homebrew Menu** | The menu that lists and starts homebrew apps (`.nro` files) on the Switch. |
| **Title takeover** | Opening the Homebrew Menu by holding **R** while starting a game, so homebrew gets all the memory a game gets. Needed for this port. |
| **`.nro`** | A homebrew app for the Switch. `NFSMostWanted2012-<version>.nro` is the port. |
| **SD card root** | The top level of the SD card, outside any folder. The `switch` folder goes there. |
