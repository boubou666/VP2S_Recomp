#!/bin/sh
# Build ps2_analyzer from the fork (mounted at /work) and run it on an ELF.
# Usage: run_analyzer.sh <input elf> <output toml>   (container paths)
# docker run --rm -v D:\VP2S_Recomp:/work vp2s-toolchain sh /work/scripts/run_analyzer.sh ...
set -e
FORK=/work/external/PS2Recomp
BUILD=$FORK/out/linux-analyzer
DEPS=""
for src in /work/build/deps/*-src; do
    [ -d "$src" ] || continue
    name=$(basename "$src" -src | tr 'a-z' 'A-Z')
    DEPS="$DEPS -DFETCHCONTENT_SOURCE_DIR_$name=$src"
done
mkdir -p "$BUILD"
cmake -S "$FORK" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release $DEPS \
    -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_ANALYZER=ON \
    > "$BUILD/configure.log" 2>&1 || { tail -20 "$BUILD/configure.log"; exit 1; }
cmake --build "$BUILD" --target ps2_analyzer > "$BUILD/build.log" 2>&1 || { grep -E "error" "$BUILD/build.log" | head -20; exit 1; }
ANALYZER=$(find "$BUILD" -name ps2_analyzer -type f | head -1)
"$ANALYZER" "$1" "$2"
