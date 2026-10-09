# Shallot vault: off-site store for the Shallot SaveMgr PC client.
# Holds only data the PC encrypted; never sees a save or a key.
FROM python:3.12.7-alpine3.20
RUN adduser -D -u 10001 vault && mkdir /data && chown vault /data
COPY shallot_vault.py /app/shallot_vault.py
USER vault
VOLUME /data
ENV SHALLOT_VAULT_DATA=/data SHALLOT_VAULT_PORT=8090
EXPOSE 8090 8091
HEALTHCHECK --interval=60s --timeout=5s CMD wget -q -O /dev/null --header "Authorization: Bearer $SHALLOT_VAULT_TOKEN" http://127.0.0.1:8090/v1/ping || exit 1
CMD ["python", "/app/shallot_vault.py"]
