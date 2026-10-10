# Contributing

Thanks for helping! Bug reports from real consoles are the most useful thing right now.

## Reporting a bug

Use the [bug report form](../../issues/new/choose) and attach **`log.txt`** from
`switch/NFSMostWanted2012/`. It says what happened at the last start, and the `perf:` lines in it
show where the frame rate goes. Please include the port version, your firmware and Atmosphère
versions, and whether you played handheld or docked.

## The one rule: no game files

Never attach, commit or link game files: the APK, the OBB, the extracted `.so` libraries or
assets, or anything taken from them. Issues and pull requests that contain or ask for game files
are closed. The `.gitignore` keeps them out of commits; please don't work around it.

## Pull requests

- Build both versions before sending: the Switch `.nro` and the Linux test build
  ([docs/BUILDING.md](docs/BUILDING.md)). The test build can run the game headless and take
  screenshots, which makes it easy to check a change.
- Keep changes focused, and describe what you tested (PC test build, console, handheld/docked).
- Code style: the existing code's (C++20, 4-space indents, comments that explain *why*).
- Changes to Dynarmic go into `patches/dynarmic-switch.patch` as well as `external/dynarmic/`.
- `external/stb` is used unchanged. Fixes for stb itself belong upstream, following stb's own rules.

By contributing you agree that your contribution is released under the project's
[MIT License](LICENSE).
