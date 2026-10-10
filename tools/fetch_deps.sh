#!/usr/bin/env bash
# Re-creates external/ from upstream if you are not using the bundled copy:
# Dynarmic (pinned commit + Switch patch) and the stb headers.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p external
if [ ! -d external/dynarmic ]; then
  git clone https://github.com/azahar-emu/dynarmic.git external/dynarmic
  git -C external/dynarmic checkout a46601580d5512d324104f985b5f0209dc980ddc
  git -C external/dynarmic submodule update --init --depth 1 \
    externals/fmt externals/mcl externals/robin-map externals/oaknut externals/xbyak externals/zycore externals/zydis
  git -C external/dynarmic apply ../../patches/dynarmic-switch.patch
fi
if [ ! -d external/stb ]; then
  git clone --depth 1 https://github.com/nothings/stb.git external/stb
fi
echo "dependencies ready"
