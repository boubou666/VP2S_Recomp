#!/bin/sh
# Run ps2_recomp on a Ghidra-exported TOML, rewriting host paths to container paths.
# Usage: run_recomp.sh <toml> <input elf> <output dir>   (container paths)
# docker run --rm -v D:\Tools\src\PS2Recomp:/src -v D:\VP2S_Recomp:/work vp2s-toolchain sh /work/scripts/run_recomp.sh ...
set -e
TOML="$1"; INPUT="$2"; OUT="$3"
DIR=$(dirname "$TOML")
CSV="${TOML%.toml}_functions.csv"
FIXED="${TOML%.toml}.docker.toml"
sed -e "s|^input = .*|input = \"$INPUT\"|" \
    -e "s|^output = .*|output = \"$OUT\"|" \
    -e "s|^ghidra_output = .*|ghidra_output = \"$CSV\"|" "$TOML" > "$FIXED"
rm -rf "$OUT"
start=$(date +%s)
cd "$DIR"
set +e
/src/out/linux/ps2xRecomp/ps2_recomp "$FIXED" > "$OUT.log" 2>&1
rc=$?
set -e
echo "exit=$rc seconds=$(( $(date +%s) - start ))"
tail -12 "$OUT.log"
echo "warnings: $(grep -ciE 'warn' "$OUT.log")  errors: $(grep -ciE 'error' "$OUT.log")"
[ -d "$OUT" ] && echo "output: $(find "$OUT" -type f | wc -l) files, $(du -sh "$OUT" | cut -f1), $(cat "$OUT"/*.cpp 2>/dev/null | wc -l) lines of C++"
