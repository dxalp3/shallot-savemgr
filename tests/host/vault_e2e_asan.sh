#!/bin/sh
# Debug helper: the VPS-only end-to-end test built with AddressSanitizer.
R="${SHALLOT_REPO:-$(cd "$(dirname "$0")/../.." && pwd)}"
T=/tmp/shallot-vault-asan
rm -rf "$T" && mkdir -p "$T"
TOKEN="e2e-token-0123456789-abcdefghij"
SHALLOT_VAULT_TOKEN="$TOKEN" SHALLOT_VAULT_DATA="$T/data" SHALLOT_VAULT_SIGNED_PORT=18091 \
    python3 "$R/vault/shallot_vault.py" 18090 >"$T/vault.log" 2>&1 &
VP=$!
sleep 1
cc -O1 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -w -c -o "$T/sqlite3.o" "$R/src/sqlite3.c"
cc -O0 -g -fsanitize=address -Wno-unused-function -Wno-format-truncation -Wno-discarded-qualifiers -I"$R/src" \
    -o "$T/asan" "$R/tests/host/test_lansync_vault.c" "$T/sqlite3.o" -lpthread -lm
SHALLOT_VAULT_E2E_PORT=18091 SHALLOT_VAULT_E2E_TOKEN="$TOKEN" "$T/asan" "$@"
kill $VP
