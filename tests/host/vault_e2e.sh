#!/bin/sh
# VPS only mode end to end (inside WSL/Linux): a real vault on localhost
# (bearer port 18090, signed-only port 18091), the PS5 code built for the
# host uploads and fetches back; writes $OUT for the PC-client cross-check:
#   py -3 tests/pc/vault_crosscheck.py <OUT> http://127.0.0.1:18090 <token>
set -eu
HERE="${SHALLOT_TESTS_DIR:-$(cd "$(dirname "$0")" && pwd)}"
TMP="${TMPDIR:-/tmp}/shallot-vault-e2e"
rm -rf "$TMP" && mkdir -p "$TMP"
TOKEN="e2e-token-0123456789-abcdefghij"
SHALLOT_VAULT_TOKEN="$TOKEN" SHALLOT_VAULT_DATA="$TMP/data" SHALLOT_VAULT_SIGNED_PORT=18091 \
    python3 "$HERE/../../vault/shallot_vault.py" 18090 >"$TMP/vault.log" 2>&1 &
VPID=$!
trap 'kill $VPID 2>/dev/null || true' EXIT
sleep 1
OUT="$TMP/test"
[ -f "$OUT-sqlite3.o" ] || cc -O1 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -w -c -o "$OUT-sqlite3.o" "$HERE/../../src/sqlite3.c"
cc -O1 -g -Wall -Wno-unused-function -Wno-format-truncation -Wno-discarded-qualifiers -I"$HERE/../../src" \
    -o "$OUT" "$HERE/test_lansync_vault.c" "$OUT-sqlite3.o" -lpthread -lm
SHALLOT_VAULT_E2E_PORT=18091 SHALLOT_VAULT_E2E_TOKEN="$TOKEN" SHALLOT_VAULT_E2E_OUT="$TMP/ps5_upload.json" "$OUT" "$@"
echo "cross-check data: $TMP/ps5_upload.json (token $TOKEN)"
if [ "${KEEP_VAULT:-0}" = "1" ]; then echo "vault left running (pid $VPID)"; trap - EXIT; wait $VPID; fi
