#!/bin/sh
# Build only the PS2Recomp recompiler (ps2_recomp) from a checkout mounted at /src.
# docker run --rm -v D:\Tools\src\PS2Recomp:/src -v D:\VP2S_Recomp:/work vp2s-toolchain sh /work/scripts/build_ps2recomp.sh
set -e
mkdir -p /src/out
cmake -S /src -B /src/out/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF \
    > /src/out/configure.log 2>&1 || { tail -20 /src/out/configure.log; exit 1; }
cmake --build /src/out/linux --target ps2_recomp -j "$(nproc)" > /src/out/build.log 2>&1 \
    || { grep -E "error|Error" /src/out/build.log | head -20; exit 1; }
ls -la /src/out/linux/ps2xRecomp/ps2_recomp
