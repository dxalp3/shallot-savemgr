#!/bin/sh
# Bind-mounted folders are often created by Docker as root: hand them to
# PUID:PGID, then run the client as that user (never as root).
set -e
if [ "$(id -u)" = "0" ]; then
    chown "${PUID}:${PGID}" /backups /config
    exec su-exec "${PUID}:${PGID}" "$@"
fi
exec "$@"
