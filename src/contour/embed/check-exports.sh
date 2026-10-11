#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Fails when the shared library named in $1 exports anything outside contour::embed, or does not
# export the two classes a host constructs. $2 is the nm to use (default: nm).
#
# The library is Contour with a small header in front of it. A symbol of Contour's that leaves it is
# one a host's own libraries can collide with, and nothing else notices when the list grows.

lib=$1
nm=${2:-nm}
[ -f "$lib" ] || { echo "check-exports: no library at '$lib'" >&2; exit 2; }

exported=$("$nm" -D --defined-only -C "$lib" | sed 's/^[0-9a-f]* *[A-Za-z] //') || exit 2
[ -n "$exported" ] || { echo "check-exports: $lib exports nothing at all" >&2; exit 1; }

foreign=$(printf '%s\n' "$exported" |
    grep -v -E '^((vtable|typeinfo|typeinfo name) for )?contour::embed::')
if [ -n "$foreign" ]; then
    count=$(printf '%s\n' "$foreign" | wc -l)
    echo "check-exports: $lib exports $count symbols outside contour::embed, the first of them:" >&2
    printf '%s\n' "$foreign" | head -10 >&2
    exit 1
fi

for wanted in 'contour::embed::Runtime::Runtime(' 'contour::embed::Terminal::Terminal(' \
              'contour::embed::Terminal::feed(' 'vtable for contour::embed::Terminal'; do
    printf '%s\n' "$exported" | grep -q -F "$wanted" ||
        { echo "check-exports: $lib does not export $wanted" >&2; exit 1; }
done
exit 0
