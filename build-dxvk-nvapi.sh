#!/bin/bash
# Clone upstream dxvk-nvapi at the pinned commit, apply the dlssg-open patch, cross-build nvapi64.dll.
# Needs: git, meson, ninja, mingw-w64-gcc, python3.
set -e
here=$(cd "$(dirname "$0")" && pwd)
base=$(cat "$here/patches/dxvk-nvapi-base-commit")
mkdir -p "$here/build"; cd "$here/build"
[ -d dxvk-nvapi ] || git clone https://github.com/jp7677/dxvk-nvapi.git
cd dxvk-nvapi
git fetch -q origin; git checkout -q "$base"; git submodule update --init --recursive -q
git checkout -q -- . 
git apply "$here/patches/dxvk-nvapi-ampere-dlssg.patch"
meson setup --cross-file build-win64.txt --buildtype release -Denable_tests=false build.w64 >/dev/null
ninja -C build.w64
cp build.w64/src/nvapi64.dll "$here/build/nvapi64.dll"
echo "built: $here/build/nvapi64.dll"
