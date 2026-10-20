#!/bin/sh
# Quick overview of the main ELF: headers, segments, sections, symbols, notable strings.
ELF=/work/extracted/SLES_546.45
OBJDUMP=mips-linux-gnu-objdump
READELF=mips-linux-gnu-readelf

echo "=== file"; file "$ELF"
echo "=== ELF header"; $READELF -h "$ELF"
echo "=== Program headers"; $READELF -l -W "$ELF"
echo "=== Sections"; $READELF -S -W "$ELF"
echo "=== Symbol count"; $READELF -s -W "$ELF" | wc -l
echo "=== First symbols"; $READELF -s -W "$ELF" | head -n 40
echo "=== Strings: source files / libraries"
mips-linux-gnu-strings -n 6 "$ELF" | grep -iE '\.(c|cpp|cc|h|s|vsm|dsm)$|sce|libsd|libgraph|libvu0|libdma|libpad|libmc|sdk|tri-ace|triace|version' | sort -u | head -n 80
echo "=== Strings: device paths / modules"
mips-linux-gnu-strings -n 5 "$ELF" | grep -iE 'cdrom|host|mc0|\.irx|\.elf|\.bin|\.dat|\.pak' | sort -u | head -n 80
