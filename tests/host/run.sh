#!/bin/sh
# Host tests for the LAN sync include files. Run inside WSL/Linux:
#   sh tests/host/run.sh [-v]
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${TMPDIR:-/tmp}/lansync-host-test"
mkdir -p "$OUT"
[ -f "$OUT/sqlite3.o" ] || cc -O1 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -w \
    -c -o "$OUT/sqlite3.o" "$HERE/../../src/sqlite3.c"
rc=0
for t in "$HERE"/test_*.c; do
    name="$(basename "$t" .c)"
    cc -O1 -g -Wall -Wno-unused-function -Wno-format-truncation -Wno-discarded-qualifiers -I"$HERE/../../src" \
        -o "$OUT/$name" "$t" "$OUT/sqlite3.o" -lpthread -lm
    printf '%s: ' "$name"
    "$OUT/$name" "$@" || rc=1
done
exit $rc
