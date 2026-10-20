#!/bin/sh
# Notable strings in the main ELF (libraries, source files, device paths, versions).
ELF=/work/extracted/SLES_546.45
S="mips-linux-gnu-strings -n 5 $ELF"
echo "=== total strings"; $S | wc -l
echo "=== SDK / libraries / versions"
$S | grep -iE 'sce|lib[a-z0-9]+|sdk|version|ver\.|copyright|tri-?ace|square' | sort -u | head -n 60
echo "=== source / asset file names"
$S | grep -iE '\.(c|cpp|cc|h|s|vsm|dsm|irx|elf|bin|dat|pak|tbl|ovl|lst)\b' | sort -u | head -n 80
echo "=== device paths"
$S | grep -iE 'cdrom|host[0-9]?:|mc[01]:|rom0|pfs|hdd' | sort -u | head -n 40
echo "=== printf-like format / debug strings sample"
$S | grep -E '%[0-9]*[dsxXf]' | head -n 50
