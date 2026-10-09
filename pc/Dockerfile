# Shallot SaveMgr PC client without a desktop: the same sync loop as the
# tray, for a NAS, home server or Docker Desktop. Backups live in /backups,
# config.json / vault.key (optional) in /config. The Shallot web UI manages it
# like the tray client (PC panel: ping, sync now, pause/resume, thin out,
# send to the VPS now).
FROM python:3.12.7-alpine3.20
RUN apk add --no-cache su-exec \
 && pip install --no-cache-dir cryptography==43.0.3 \
 && adduser -D -u 10001 shallot && mkdir /backups /config && chown shallot /backups /config
COPY shallot_sync.py /app/shallot_sync.py
COPY docker-entrypoint.sh /app/docker-entrypoint.sh
RUN chmod 0755 /app/docker-entrypoint.sh
VOLUME ["/backups", "/config"]
# PUID/PGID: owner of the files in /backups and /config (match your host user on Linux).
ENV SHALLOT_BACKUP_DIR=/backups SHALLOT_LOG_FILE=/config/shallot_sync.log SHALLOT_VAULT_KEY_FILE=/config/vault.key \
    SHALLOT_CONTAINER=1 PUID=10001 PGID=10001 PYTHONUNBUFFERED=1
HEALTHCHECK --interval=60s --timeout=5s --start-period=30s CMD test -f /config/shallot_sync.log || exit 1
ENTRYPOINT ["/app/docker-entrypoint.sh"]
CMD ["python", "/app/shallot_sync.py", "--config", "/config/config.json"]
