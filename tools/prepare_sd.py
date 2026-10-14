#!/usr/bin/env python3
"""Set up the NFS Most Wanted 2012 port's folder on the SD card from YOUR copy of the game.

Optional since 0.2.5: copying the APK and the OBB into the port's game/ folder is enough,
the port then does the same on the console at its first start. This script does it on a
computer instead, and can also give the port the game's icon (--nro).

The port ships no game files. You need two files from the Android game
(version 1.3.128, package com.ea.games.nfs13_row):

  APK  the app (e.g. NFS_MW.apk). It is NOT copied to the SD card: this script
       takes the 32-bit ARM libraries (lib/armeabi-v7a/*.so) and the assets/
       folder out of it.
  OBB  main.1003128.com.ea.games.nfs13_row.obb, the game data (cars, tracks,
       textures, sounds). It is copied UNCHANGED, as one file. Do not unzip it:
       the game opens the archive itself.

With --nro, the port (NFSMostWanted2012-<version>.nro) is copied into the folder
with the game's own icon (taken from your APK) instead of the port's plain one,
so the Homebrew Menu shows the game's icon. That needs Pillow (pip install
pillow). The icon is only put on your own copy: the released .nro contains no
game artwork.

Result (--out is the port's folder on the SD card, normally sd:/switch/NFSMostWanted2012):

    NFSMostWanted2012-<version>.nro   the port (with --nro)
    game/                         files from your copy of the game
      main.1003128.com.ea.games.nfs13_row.obb   the OBB, unchanged
      lib/                        the 5 armeabi-v7a libraries from the APK
      assets/                     the APK's assets/ folder
    save/progress/, save/settings/   written by the game (your progress)
    settings.ini, log.txt         created by the port

Usage:
    python3 prepare_sd.py --apk NFS_MW.apk --obb main.1003128.com.ea.games.nfs13_row.obb --nro NFSMostWanted2012-0.2.7.nro --out E:/switch/NFSMostWanted2012
"""
import argparse
import hashlib
import io
import os
import re
import shutil
import struct
import sys
import zipfile

PACKAGE = "com.ea.games.nfs13_row"
VERSION_CODE = 1003128
OBB_NAME = f"main.{VERSION_CODE}.{PACKAGE}.obb"
REQUIRED_LIBS = ["libapp.so", "libc++_shared.so", "libfmodex.so", "libfmodevent.so", "libNimble.so"]
# libapp.so builds this loader has been tested with (SHA-256).
KNOWN_LIBAPP = {
    "a73dcc170552866af52bf2d4f5822606307fb42aed1ef0df2e469fdfa374ff43": "1.3.128 (armeabi-v7a)",
}

def check_obb_archive(z, where):
    """The OBB is a zip archive whose entries start with published.*/ (textures, cars, tracks...)."""
    names = z.namelist()
    if not any(n.startswith("published") for n in names):
        print(f"WARNING: {where} does not look like the NFS Most Wanted OBB (no published.*/ entries). "
              "Using it anyway.")
    return len(names)


def open_apk(path):
    if os.path.isdir(path):
        sys.exit(f"--apk {path} is a folder. Pass the .apk file itself, not an extracted copy.")
    if not os.path.isfile(path):
        sys.exit(f"--apk {path}: file not found.")
    if not zipfile.is_zipfile(path):
        sys.exit(f"--apk {path} is not an APK (an APK is a zip archive).")
    z = zipfile.ZipFile(path)
    names = z.namelist()
    if not any(n.startswith("lib/") for n in names) and any(n.endswith(".apk") for n in names):
        sys.exit(f"{path} is an app bundle (.xapk/.apks), not an APK.\n"
                 "Unzip the bundle first (it is a zip archive), then run this script with the .apk and\n"
                 f"the {OBB_NAME} found inside it.")
    return z


def validate_obb(src):
    if os.path.isdir(src):
        sys.exit(f"--obb {src} is a folder. Pass the .obb file itself.\n"
                 "Do not unzip the OBB: the game reads the archive directly, so extracted files are not used.")
    if not os.path.isfile(src):
        sys.exit(f"--obb {src}: file not found.")
    if not zipfile.is_zipfile(src):
        sys.exit(f"--obb {src} is not a valid OBB: it must be the original file as installed by the game\n"
                 "(a zip archive), not extracted, converted or renamed from something else.")
    with zipfile.ZipFile(src) as z:
        return check_obb_archive(z, src)


def copy_obb(src, dest, n):
    if os.path.basename(src) != OBB_NAME:
        print(f"note: the OBB is named {os.path.basename(src)}; it is saved as {OBB_NAME}")
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.copyfile(src, dest)
    print(f"copied OBB unchanged ({os.path.getsize(dest) / 2**20:.0f} MiB, {n} entries) -> {dest}")


# ---- the game's icon on the loader --------------------------------------------------
DENSITY_ORDER = ["xxxhdpi", "xxhdpi", "xhdpi", "hdpi", "mdpi", ""]


def _by_density(names, filename):
    """res/drawable-<density>[-vN]/<filename>, highest density first."""
    found = []
    for n in names:
        m = re.fullmatch(r"res/(?:drawable|mipmap)(?:-([a-z]+dpi))?(?:-v\d+)?/" + re.escape(filename), n)
        if m:
            d = m.group(1) or ""
            found.append((DENSITY_ORDER.index(d) if d in DENSITY_ORDER else len(DENSITY_ORDER), n))
    return [n for _, n in sorted(found)]


def game_icon_jpeg(z):
    """The game's launcher icon from the APK as a 256x256 JPEG (what the Homebrew Menu shows).

    Uses the adaptive icon (background + foreground layers, the visible middle 72/108 of
    each side) when the APK has it, else the largest plain icon. Returns None if Pillow is
    missing or no icon is found."""
    try:
        from PIL import Image
    except ImportError:
        print("note: Pillow is not installed (pip install pillow), so the .nro keeps its own icon")
        return None
    names = z.namelist()
    bg, fg = _by_density(names, "ic_background.png"), _by_density(names, "ic_foreground.png")
    img = None
    if bg and fg:
        b = Image.open(io.BytesIO(z.read(bg[0]))).convert("RGBA")
        f = Image.open(io.BytesIO(z.read(fg[0]))).convert("RGBA").resize(b.size, Image.LANCZOS)
        full = Image.alpha_composite(b, f)
        m = round(full.size[0] * 18 / 108)
        img = full.crop((m, m, full.size[0] - m, full.size[1] - m))
    else:
        for name in ("icon.png", "ic_launcher.png"):
            cands = _by_density(names, name)
            if cands:
                im = Image.open(io.BytesIO(z.read(cands[0]))).convert("RGBA")
                box = im.split()[3].getbbox() or (0, 0, *im.size)
                im = im.crop(box)
                white = Image.new("RGBA", im.size, (255, 255, 255, 255))
                img = Image.alpha_composite(white, im)
                break
    if img is None:
        print("note: no launcher icon found in the APK, so the .nro keeps its own icon")
        return None
    out = io.BytesIO()
    img.convert("RGB").resize((256, 256), Image.LANCZOS).save(out, "JPEG", quality=92, optimize=True)
    return out.getvalue()


def set_nro_icon(nro_bytes, jpeg):
    """Replace the icon in an NRO's asset section (ASET: icon, NACP, RomFS)."""
    if nro_bytes[0x10:0x14] != b"NRO0":
        raise ValueError("not an NRO file")
    size = struct.unpack_from("<I", nro_bytes, 0x18)[0]
    aset = nro_bytes[size:]
    if aset[:4] != b"ASET":
        raise ValueError("the NRO has no asset section")
    ver = struct.unpack_from("<I", aset, 4)[0]
    sections = [struct.unpack_from("<QQ", aset, 8 + 16 * i) for i in range(3)]  # icon, nacp, romfs
    blobs = [jpeg] + [aset[o:o + n] for o, n in sections[1:]]
    head = 8 + 16 * 3
    body, entries, pos = b"", [], head
    for blob in blobs:
        entries.append((pos if blob else 0, len(blob)))
        body += blob
        pos += len(blob)
    new = b"ASET" + struct.pack("<I", ver) + b"".join(struct.pack("<QQ", o, n) for o, n in entries)
    return nro_bytes[:size] + new + body


def install_nro(src, dest, z):
    data = open(src, "rb").read()
    jpeg = game_icon_jpeg(z)
    if jpeg:
        try:
            data = set_nro_icon(data, jpeg)
            print(f"{os.path.basename(dest)}: icon replaced with the game's icon from your APK")
        except ValueError as e:
            print(f"note: could not change the icon ({e}); the .nro keeps its own icon")
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = dest + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, dest)
    print(f"copied the port -> {dest}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apk", required=True, help="the game's Android APK file")
    ap.add_argument("--obb", help=f"the game's OBB data file ({OBB_NAME}), as a single file; not extracted")
    ap.add_argument("--nro", help="the port (NFSMostWanted2012-<version>.nro) to copy into the folder, with the game's icon from your APK")
    ap.add_argument("--out", required=True, help="the port's folder (e.g. <SD>/switch/NFSMostWanted2012)")
    a = ap.parse_args()
    if a.nro and not os.path.isfile(a.nro):
        sys.exit(f"--nro {a.nro}: file not found.")

    obb_entries = validate_obb(a.obb) if a.obb else 0  # check the inputs before writing anything
    out = os.path.abspath(a.out)
    game = os.path.join(out, "game")
    lib_dir, assets_dir = os.path.join(game, "lib"), os.path.join(game, "assets")
    obb_dest = os.path.join(game, OBB_NAME)
    old_layout = [p for p in ("lib", "apk", "fs", OBB_NAME) if os.path.exists(os.path.join(out, p))]

    with open_apk(a.apk) as z:
        names = z.namelist()
        libs = {n.split("/")[-1]: n for n in names if n.startswith("lib/armeabi-v7a/") and n.endswith(".so")}
        missing = [l for l in REQUIRED_LIBS if l not in libs]
        if missing:
            sys.exit("This APK has no 32-bit ARM (armeabi-v7a) build of: " + ", ".join(missing) +
                     "\nThe loader runs the armeabi-v7a libraries; use an APK that contains them.")
        os.makedirs(lib_dir, exist_ok=True)
        for l in REQUIRED_LIBS:
            with z.open(libs[l]) as src, open(os.path.join(lib_dir, l), "wb") as dst:
                shutil.copyfileobj(src, dst)
        print(f"copied the 5 armeabi-v7a libraries -> {lib_dir}")
        digest = hashlib.sha256(open(os.path.join(lib_dir, "libapp.so"), "rb").read()).hexdigest()
        print(f"libapp.so sha256 {digest}: " + KNOWN_LIBAPP.get(digest, "UNTESTED build (it may still work)"))

        n_assets = 0
        for n in names:
            if n.startswith("assets/") and not n.endswith("/"):
                dest = os.path.join(game, n)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with z.open(n) as src, open(dest, "wb") as dst:
                    shutil.copyfileobj(src, dst)
                n_assets += 1
        print(f"extracted {n_assets} asset files -> {assets_dir}")

        # Some repacks carry the OBB inside the APK.
        inner = [n for n in names if n.endswith(".obb")]
        if inner and not a.obb:
            os.makedirs(os.path.dirname(obb_dest), exist_ok=True)
            with z.open(inner[0]) as src, open(obb_dest, "wb") as dst:
                shutil.copyfileobj(src, dst)
            if not zipfile.is_zipfile(obb_dest):
                sys.exit(f"the OBB inside the APK ({inner[0]}) is not a valid OBB archive.")
            with zipfile.ZipFile(obb_dest) as oz:
                check_obb_archive(oz, inner[0])
            print(f"took the OBB out of the APK unchanged: {inner[0]} -> {obb_dest}")

        if a.nro:
            install_nro(a.nro, os.path.join(out, os.path.basename(a.nro)), z)

    if a.obb:
        copy_obb(a.obb, obb_dest, obb_entries)
    if not os.path.exists(obb_dest):
        print(f"WARNING: no OBB data file. The game cannot start without it: copy {OBB_NAME}\n"
              f"unchanged into {game}, or run again with --obb")
    if old_layout:
        print("note: this folder also has files of the earlier layout (" + ", ".join(old_layout) + "); the port\n"
              "moves them into game/ and save/ the first time it starts, keeping your progress")
    print(f"done: {out}")
    # The port's folder under its earlier names, next to this one.
    for prev in ("NFSMostWanted", "nfsmw"):
        p = os.path.join(os.path.dirname(out), prev)
        if os.path.normcase(p) != os.path.normcase(out) and os.path.isdir(p):
            print(f"note: {p} is the port's folder from before it was renamed. The port moves your\n"
                  "progress and settings from it into this folder the first time it starts, and removes\n"
                  "the old version; anything left there afterwards can be deleted.")
    nros = sorted(n for n in os.listdir(out) if n.lower().endswith(".nro"))
    if not nros:
        print(f"now copy the port (NFSMostWanted2012-<version>.nro) into {out}")
    elif len(nros) > 1:
        keep = os.path.basename(a.nro) if a.nro else None
        old = [n for n in nros if n != keep]
        print("note: this folder has more than one version of the port (" + ", ".join(nros) + ").\n"
              "Delete " + (", ".join(old) if keep else "the older ones") +
              " so the Homebrew Menu shows only the new version; your progress is not affected.")


if __name__ == "__main__":
    main()
