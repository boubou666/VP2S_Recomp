#!/bin/sh
# Build and run the PS2Recomp test suite (ps2x_tests) from a checkout mounted at /src.
# docker run --rm -v D:\VP2S_Recomp\external\PS2Recomp:/src -v D:\VP2S_Recomp:/work vp2s-toolchain sh /work/scripts/build_ps2recomp_tests.sh [build dir name]
# Optional: mount a previous build's _deps at /deps to reuse dependency sources offline
# (-v <build>/_deps:/deps:ro).
set -e
BUILD="/src/out/${1:-linux-tests}"
mkdir -p "$BUILD"
DEPS_ARGS=""
if [ -d /deps ]; then
    for src in /deps/*-src; do
        name=$(basename "$src" -src | tr 'a-z' 'A-Z')
        DEPS_ARGS="$DEPS_ARGS -DFETCHCONTENT_SOURCE_DIR_$name=$src"
    done
fi
# The runtime uses SSE4.1 intrinsics; MSVC accepts them as is, g++ needs the target flag.
cmake -S /src -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release $DEPS_ARGS \
    -DCMAKE_C_FLAGS="-march=x86-64-v2" -DCMAKE_CXX_FLAGS="-march=x86-64-v2" \
    -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_FFMPEG=OFF -DPS2X_ENABLE_DEBUG_UI=OFF \
    -DPS2X_ENABLE_SCCACHE=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF \
    > "$BUILD/configure.log" 2>&1 || { tail -30 "$BUILD/configure.log"; exit 1; }
cmake --build "$BUILD" --target ps2x_tests ps2_recomp -j "$(nproc)" > "$BUILD/build.log" 2>&1 \
    || { grep -E "error" "$BUILD/build.log" | head -30; exit 1; }
cd "$BUILD"
TESTS=$(find . -name ps2x_tests -type f | head -1)
set +e
"$TESTS" > tests.log 2>&1
rc=$?
set -e
echo "ps2x_tests exit=$rc"
tail -15 tests.log
