#!/bin/sh
# Clone the project (mounted read-only at /repo) with its submodules into a fresh
# directory and check the PS2Recomp fork commit is fetched from GitHub.
# docker run --rm -v D:\VP2S_Recomp:/repo:ro vp2s-toolchain sh /repo/scripts/check_fresh_clone.sh [branch]
set -e
BRANCH="${1:-main}"
git config --global --add safe.directory '*'
git clone -q --branch "$BRANCH" --recurse-submodules /repo /tmp/clone
cd /tmp/clone
git submodule status
git -C external/PS2Recomp log --oneline -1
git -C external/PS2Recomp remote get-url origin
test -f external/PS2Recomp/ps2xRuntime/include/ps2_overlay_modules.h && echo "overlay sources present"
