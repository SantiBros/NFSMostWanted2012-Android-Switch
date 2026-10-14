#!/usr/bin/env python3
"""Pack the port into one zip for a GitHub release: dist/NFSMostWanted2012-<version>.zip

    python3 tools/make_release.py [--nro build-switch/NFSMostWanted2012-<version>.nro]

The version comes from NFSMW_VERSION in CMakeLists.txt.

The zip contains no game files, only what every player needs plus the source:

    switch/NFSMostWanted2012/            goes on the SD card:
      NFSMostWanted2012-<version>.nro    the port (plain icon)
      README.txt                         the guide: the folder, install, controls,
                                         troubleshooting, what's new, build checksum
      LICENSES.txt                       the open-source components and their licenses
      game/                              empty: the player's own game files go here
      save/progress/, save/settings/     empty: the game's saves go here
      source/                            the source code (the files git tracks, or the
                                         project's folders when it is not a git checkout)
    prepare_sd.py                        fills game/ from the player's APK and OBB
"""
import argparse
import datetime
import fnmatch
import hashlib
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
# The port's name on the SD card: switch/<NAME>/<NAME>-<version>.nro, and the zip's.
# "2012" tells it apart from the port of NFS Most Wanted (2005).
NAME = "NFSMostWanted2012"
ROOT = os.path.dirname(HERE)

# Without git: the project's own files and folders, minus build output.
SOURCE_ITEMS = ["CMakeLists.txt", "README.md", "README.es.md", "CHANGELOG.md", "THIRD_PARTY.md", "CONTRIBUTING.md", "LICENSE",
                ".gitignore", ".gitattributes", ".github", "cmake", "docs", "external/dynarmic", "external/stb",
                "patches", "res", "src", "tools"]
# Not packed into source/: the README's pictures (they only matter on GitHub).
SKIP_PREFIXES = ["docs/images/"]
SKIP_DIRS = {".git", "__pycache__", "build", "dist"}
# Never packed, whatever the source of the file list: game files and run output.
NEVER = ["*.apk", "*.xapk", "*.apks", "*.obb", "*.so", "*.sba", "*.nro", "*.nacp", "*.elf", "*.o", "*.a",
         "log.txt", "nfsmw.log", "settings.ini"]


def crlf_text(text):
    """Text for Windows Notepad as well: CRLF line ends."""
    return text.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8")


def crlf(path, **subst):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    for k, v in subst.items():
        text = text.replace("@" + k + "@", v)
    return crlf_text(text)


# The open-source code built into the .nro, with where its license text is
# (relative to the source tree, or res/licenses/ for what devkitPro provides).
COMPONENTS = [
    ("Dynarmic", "ARM-to-ARM64 recompiler that runs the game's code", "0BSD", "external/dynarmic/LICENSE.txt"),
    ("fmt", "text formatting (used by Dynarmic)", "MIT", "external/dynarmic/externals/fmt/LICENSE"),
    ("mcl", "utility library (used by Dynarmic)", "MIT", "external/dynarmic/externals/mcl/LICENSE"),
    ("oaknut", "ARM64 code generator (used by Dynarmic)", "MIT", "external/dynarmic/externals/oaknut/LICENSE"),
    ("robin-map", "hash tables (used by Dynarmic)", "MIT", "external/dynarmic/externals/robin-map/LICENSE"),
    ("Boost (ICL, Variant)", "header-only libraries (used by Dynarmic)", "BSL-1.0", "res/licenses/boost.txt"),
    ("stb (stb_truetype, stb_image)", "font rendering; unpacking the APK", "MIT or public domain", "external/stb/LICENSE"),
    ("zlib (inffast.c, adapted)", "decompressing the game data natively", "zlib", "res/licenses/zlib.txt"),
    ("libnx", "Nintendo Switch homebrew library", "ISC", "res/licenses/libnx.txt"),
    ("Mesa", "OpenGL ES driver (switch-mesa)", "MIT", "res/licenses/mesa.txt"),
    ("libdrm_nouveau", "GPU interface for Mesa", "MIT", "res/licenses/libdrm_nouveau.txt"),
    ("newlib", "C library (devkitA64)", "BSD-style, several authors", "res/licenses/newlib.txt"),
    ("GCC runtime (libstdc++, libgcc)", "C++ runtime (devkitA64)",
     "GPL-3.0 with the GCC Runtime Library Exception", "res/licenses/gcc-runtime-exception.txt"),
]


def licenses_text(source):
    out = ["NFS MOST WANTED 2012 FOR NINTENDO SWITCH - OPEN-SOURCE COMPONENTS",
           "==================================================================",
           "",
           "The port (the .nro file) is built with the following open-source software,",
           "with thanks to their authors. Their licenses follow below.",
           ""]
    for name, what, lic, _ in COMPONENTS:
        out.append(f"  {name:<32} {lic}")
        out.append(f"  {'':<32} {what}")
    out += ["", "The game itself (Need for Speed: Most Wanted) is not included and belongs to",
            "Electronic Arts Inc.", ""]
    for name, what, lic, path in COMPONENTS:
        p = os.path.join(source, path)
        if not os.path.isfile(p):
            p = os.path.join(ROOT, path)
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            body = f.read().strip()
        title = f"{name} ({lic})"
        out += ["", "-" * 79, title, "-" * 79, "", body, ""]
    return "\n".join(out) + "\n"


def project_version():
    cmake = open(os.path.join(ROOT, "CMakeLists.txt"), encoding="utf-8").read()
    m = re.search(r'set\(NFSMW_VERSION\s+"([^"]+)"\)', cmake)
    if not m:
        sys.exit("CMakeLists.txt: set(NFSMW_VERSION \"x.y.z\") not found")
    return m.group(1)


def source_files(src):
    try:
        out = subprocess.run(["git", "-C", src, "ls-files", "-z"], capture_output=True, check=True).stdout
        files = [f for f in out.decode().split("\0") if f]
        if files:
            return files
    except (OSError, subprocess.CalledProcessError):
        pass
    files = []
    for item in SOURCE_ITEMS:
        p = os.path.join(src, item)
        if os.path.isfile(p):
            files.append(item)
            continue
        for d, dirs, names in os.walk(p):
            dirs[:] = sorted(x for x in dirs if x not in SKIP_DIRS and not x.startswith("build"))
            for n in sorted(names):
                files.append(os.path.relpath(os.path.join(d, n), src).replace(os.sep, "/"))
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nro", help="the Switch build (default: build-switch/NFSMostWanted2012-<version>.nro)")
    ap.add_argument("--source", default=ROOT, help="project folder to pack as switch/NFSMostWanted2012/source/ (default: this one)")
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    a = ap.parse_args()
    version = project_version()
    nro_name = f"{NAME}-{version}.nro"
    a.nro = a.nro or os.path.join(ROOT, "build-switch", nro_name)
    if not os.path.isfile(a.nro):
        sys.exit(f"{a.nro} not found: build the Switch version first (see README, Building for Switch)")
    with open(a.nro, "rb") as f:
        nro = f.read()
    if nro[0x10:0x14] != b"NRO0":
        sys.exit(f"{a.nro} is not an NRO file")

    files = [f for f in source_files(a.source)
             if not any(fnmatch.fnmatch(os.path.basename(f), pat) for pat in NEVER)
             and not any(f.startswith(p) for p in SKIP_PREFIXES)]
    os.makedirs(a.out, exist_ok=True)
    zpath = os.path.join(a.out, f"{NAME}-{version}.zip")
    res = os.path.join(ROOT, "res")
    stamp = datetime.date.today().isoformat()
    sha = hashlib.sha256(nro).hexdigest()
    d = f"switch/{NAME}/"
    def folder(z, name):  # an empty folder in the zip
        info = zipfile.ZipInfo(name.rstrip("/") + "/")
        info.external_attr = (0o40755 << 16) | 0x10
        z.writestr(info, b"")

    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(d + nro_name, nro)
        z.writestr(d + "README.txt", crlf(os.path.join(res, "switch-README.txt"), VERSION=version, DATE=stamp, SHA256=sha))
        z.writestr(d + "LICENSES.txt", crlf_text(licenses_text(a.source)))
        for f in ("game", "save", "save/progress", "save/settings"):
            folder(z, d + f)
        z.write(os.path.join(HERE, "prepare_sd.py"), "prepare_sd.py")
        for f in files:
            z.write(os.path.join(a.source, f), d + "source/" + f)
    print(f"release: {zpath} ({os.path.getsize(zpath) / 2**20:.1f} MiB, {len(files)} source files)")
    print(f"{nro_name} SHA-256 {sha}")


if __name__ == "__main__":
    main()
