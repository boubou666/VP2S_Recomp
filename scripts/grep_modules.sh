#!/bin/sh
# Grep strings of extracted modules.
# Usage: RE=<regex> grep_modules.sh <glob...>   (globs are expanded here, regex comes from $RE)
for f in $@; do
    echo "== ${f##*/}"
    mips-linux-gnu-strings -n 5 "$f" | grep -iE "$RE" | sort -u | head -25
done
