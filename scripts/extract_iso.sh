#!/bin/sh
# Lists the ISO and extracts its contents to /work/extracted (skips if already done).
set -e
ISO=$(ls /work/*.iso | head -n 1)
echo "ISO: $ISO"
7z l "$ISO" | tail -n +12
if [ ! -f /work/extracted/SYSTEM.CNF ]; then
    7z x -y -o/work/extracted "$ISO" > /dev/null
fi
echo "--- SYSTEM.CNF"
cat /work/extracted/SYSTEM.CNF
