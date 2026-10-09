#!/bin/sh
# Build the current working tree (including uncommitted changes) inside WSL.
# Usage from Windows (strips CR from this script, passes the repo path):
#   R=/mnt/c/Users/s4cha/OneDrive/Desktop/Ancillary/ShallotProject/shallot-savemgr
#   wsl.exe -d archlinux -- sh -c "sed 's/\r$//' $R/tools/wsl-build.sh | sh -s $R"
# Copies Makefile + src/ to a Linux-native dir (avoids CRLF/9p issues),
# builds there, and copies shallot-savemgr.elf back to the repo root.
set -eu
SRC_DIR="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
BUILD_DIR="${BUILD_DIR:-/root/shallot-wt-build}"
SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cp -r "$SRC_DIR/Makefile" "$SRC_DIR/src" "$BUILD_DIR/"
mkdir -p "$BUILD_DIR/pc"
cp "$SRC_DIR/pc/shallot_sync.py" "$SRC_DIR/pc/Dockerfile" "$SRC_DIR/pc/docker-entrypoint.sh" \
   "$SRC_DIR/pc/docker-compose.yml" "$BUILD_DIR/pc/"
mkdir -p "$BUILD_DIR/vault"
cp "$SRC_DIR/vault/shallot_vault.py" "$SRC_DIR/vault/Dockerfile" "$SRC_DIR/vault/docker-compose.yml" \
   "$SRC_DIR/vault/Caddyfile" "$BUILD_DIR/vault/"
mkdir -p "$BUILD_DIR/assets"
cp "$SRC_DIR/assets/icon0.png" "$BUILD_DIR/assets/"
rm -f "$BUILD_DIR/src/ui.h" "$BUILD_DIR/src/pc_client.h" "$BUILD_DIR/src/launcher_icon.h" "$BUILD_DIR/src/sqlite3.o"
# Make recipes must not carry CR.
sed -i 's/\r$//' "$BUILD_DIR/Makefile"

cd "$BUILD_DIR"
make PS5_PAYLOAD_SDK="$SDK"
cp shallot-savemgr.elf "$SRC_DIR/shallot-savemgr.elf"
echo "OK: $SRC_DIR/shallot-savemgr.elf"
