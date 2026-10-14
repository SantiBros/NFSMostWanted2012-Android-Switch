# Third-party software

The port is built with the following open-source software, with thanks to their authors. The
release zip's `LICENSES.txt` has the full license text of everything that goes into the `.nro`.

## Built into the `.nro`

| Component | Used for | License | Source / license text |
|---|---|---|---|
| [Dynarmic](https://github.com/azahar-emu/dynarmic) | Recompiles the game's 32-bit ARM code (with `patches/dynarmic-switch.patch`) | 0BSD | `external/dynarmic/LICENSE.txt` |
| [oaknut](https://github.com/merryhime/oaknut) | ARM64 code generator (used by Dynarmic) | MIT | `external/dynarmic/externals/oaknut/LICENSE` |
| [mcl](https://github.com/merryhime/mcl) | Utility library (used by Dynarmic) | MIT | `external/dynarmic/externals/mcl/LICENSE` |
| [{fmt}](https://github.com/fmtlib/fmt) | Text formatting (used by Dynarmic) | MIT | `external/dynarmic/externals/fmt/LICENSE` |
| [robin-map](https://github.com/Tessil/robin-map) | Hash tables (used by Dynarmic) | MIT | `external/dynarmic/externals/robin-map/LICENSE` |
| [Boost](https://www.boost.org) (ICL, Variant; headers only) | Used by Dynarmic | BSL-1.0 | `res/licenses/boost.txt` |
| [stb](https://github.com/nothings/stb) (`stb_truetype`, `stb_image`) | Font rendering; unpacking the APK at the first start | MIT or public domain | `external/stb/LICENSE` |
| [zlib](https://zlib.net) (`inffast.c` 1.2.8, adapted in `src/game/native.cpp`) | Decompressing the game data natively | zlib | `res/licenses/zlib.txt` |
| [libnx](https://github.com/switchbrew/libnx) | Nintendo Switch homebrew library | ISC | `res/licenses/libnx.txt` |
| [Mesa](https://mesa3d.org) (switch-mesa) | OpenGL ES driver | MIT | `res/licenses/mesa.txt` |
| libdrm_nouveau | GPU interface for Mesa | MIT | `res/licenses/libdrm_nouveau.txt` |
| [newlib](https://sourceware.org/newlib/) (devkitA64) | C library | BSD-style, several authors | `res/licenses/newlib.txt` |
| GCC runtime (libstdc++, libgcc; devkitA64) | C++ runtime | GPL-3.0 with the GCC Runtime Library Exception | `res/licenses/gcc-runtime-exception.txt` |

libnx, Mesa, libdrm_nouveau, newlib and the GCC runtime come from
[devkitPro](https://devkitpro.org)'s packages and are not in this repository.

## Only in the PC test build

| Component | Used for | License | License text |
|---|---|---|---|
| [xbyak](https://github.com/herumi/xbyak) | x86-64 code generator (used by Dynarmic) | BSD-3-Clause | `external/dynarmic/externals/xbyak/COPYRIGHT` |
| [Zydis](https://github.com/zyantific/zydis), [Zycore](https://github.com/zyantific/zycore-c) | x86 disassembler (Dynarmic debugging) | MIT | `external/dynarmic/externals/zydis/LICENSE`, `.../zycore/LICENSE` |
| `stb_image_write` | Screenshots of the test build | MIT or public domain | `external/stb/LICENSE` |

## Artwork in this repository

- The banner, the social preview and the port's icon (`res/icon.jpg`, `docs/images/`) use the font
  [Barlow Condensed](https://github.com/jpt/barlow) by Jeremy Tribby, under the SIL Open Font
  License 1.1. The font itself is not included.
- The screenshots in `docs/images/` show the game running in the port, to illustrate it. The game,
  its graphics and its brands belong to Electronic Arts Inc.

## The game

*Need for Speed: Most Wanted* is © Electronic Arts Inc. None of its code or data is included in
this repository or in the releases: every player uses their own copy.
