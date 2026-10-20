#!/bin/sh
# Count instructions matching a mnemonic regex in every extracted module and the main ELF.
# Usage: RE=<regex> count_mnemonics.sh
for f in /work/out/modules/*.bin /work/out/iso/SLES_546.45; do
    n=$(mips-linux-gnu-objdump -D -b binary -m mips:5900 -EL "$f" 2>/dev/null | awk -F'\t' '{print $3}' | grep -cE "$RE")
    [ "$n" -gt 0 ] && echo "${f##*/} $n"
done
