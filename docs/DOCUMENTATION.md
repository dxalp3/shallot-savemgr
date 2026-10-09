# Shallot SaveMgr — documentation

Shallot runs on a jailbroken PS5 as a payload. It serves a web UI on port
8082 and backs up game saves automatically, keeping a history of versions you
can restore. Backups can stay on the console or go to a PC on your network,
your own server (a "VPS vault"), or both. It is a fork of
[Garlic SaveMgr](https://git.etawen.dev/earthonion/garlic-savemgr) by
earthonion, whose save tools (browse, decrypt, encrypt, resign, import) it
keeps.

Contents

1. [How it works](#1-how-it-works)
2. [Installing and starting](#2-installing-and-starting)
3. [Concepts](#3-concepts)
4. [When backups happen](#4-when-backups-happen)
5. [Where backups go](#5-where-backups-go)
6. [Restoring](#6-restoring)
7. [The PC client](#7-the-pc-client)
8. [The VPS vault](#8-the-vps-vault)
9. [The web UI](#9-the-web-ui)
10. [Security](#10-security)
11. [Settings reference](#11-settings-reference)
12. [Files and paths](#12-files-and-paths)
13. [HTTP API](#13-http-api)
14. [Troubleshooting](#14-troubleshooting)
15. [Development](#15-development)
16. [Testing status](#16-testing-status)

---

## 1. How it works

```
 PS5 (Shallot payload, port 8082)
 ├─ watches games: on close → waits for stable saves → makes a backup (ZIP)
 ├─ keeps a state database (versions, restore points, deliveries)
 ├─ queue: backups waiting for their destinations
 │
 ├──► PC client (tray app or Docker) ── long-polls the PS5, downloads, verifies
 │       └─(optional)──► VPS vault  (encrypted on the PC)
 └──► VPS vault (encrypted on the PS5, signed HTTP, port 8091)
```

A backup is an exact ZIP of a save's files plus a SHA-256. Every backup is
also recorded as a *version* of its save, and each backup run of a game
records a *restore point* (which version of every save belonged together).
The PS5 deletes its own copy of a backup only after every destination has
confirmed it (or, in PS5-only mode, thins and deduplicates it on the console).

## 2. Installing and starting

1. Send `shallot-savemgr.elf` to your payload loader (any ELF loader works).
2. Open `http://<ps5-ip>:8082` from a PC or phone, or from the PS5 itself via
   the **Shallot** tile on the home screen. The tile is added on first start
   (Settings → Home screen to remove or add it). It only opens the page:
   Shallot must be running.
3. To start Shallot with the console, either:
   - **onionHEN**: Settings → Start with the console → choose the ELF file
     for Shallot on a PC → *Store this file on the PS5* → *Start with the
     console*. Shallot keeps a copy and puts it in onionHEN's payload folder
     with its auto-start marker;
   - **any other loader** that runs payloads at boot: add the ELF there, as
     that loader describes.

**Stopping**: the *Stop Shallot* button offers **Pause** (automatic backups
off; the page and the tile keep working; *Resume* in the header) or **Quit**
(ends the payload after a running backup or restore finishes, at most 30 s).

**Updating** Shallot: Quit, then send the new ELF. A second copy started while
one runs cannot bind port 8082 and exits.

## 3. Concepts

- **Save / save object.** A game's save as you see it in game. On disk it is a
  save file (`sdimg_<name>`) and usually the PS5's own backup copy of it
  (`sdimg_sce_bu_<name>`); Shallot backs them up and restores them together.
  You can give a save a name (presets or your own).
- **Backup (archive).** One ZIP per save file, with its SHA-256, kept in the
  queue until the destinations have it.
- **Version (generation).** A backup that holds content not seen before.
  Unchanged saves are not backed up again.
- **Restore point (snapshot).** Which version of each save of a game existed
  at one moment, e.g. after a game closed. Restoring a restore point brings
  the whole game back to that moment, including removing saves that did not
  exist then.
- **History thinning.** Old versions are reduced on a schedule: every version
  for the first days, then one per day, then one per week (configurable).
  The newest version of every save is always kept.
- **Deduplication.** Old versions are stored as 4 KiB blocks; blocks that did
  not change between versions are stored once.

## 4. When backups happen

| Trigger | What happens |
|---|---|
| A game closes | Shallot waits until the saves stop changing (stability check: *samples* × *interval*, up to a maximum wait), then backs up what changed. |
| Re-check after close | The PS5 rewrites its own backup copy of a save a few minutes after a game closes; a second look (default 180 s) catches it. |
| Idle sweep | When no game has run for the idle delay (default 10 min), all saves are scanned for changes. |
| Startup | Saves changed while Shallot was not running are backed up; games never backed up get a first backup. |
| By hand | Backup & Sync → *Scan Changed Saves Now*, a game's *Back up now*, a save's *Back up now*. |
| Power-off / rest | On a power-off signal Shallot finishes the backup in progress and starts nothing risky. After rest mode it resumes and catches up. |

A running game is never backed up (its saves may be half written); it is
backed up when it closes.

**Older version detected.** If a save's content goes back to an older version
(a crash, a cloud download), Shallot records it under *Recovery* and, by
setting, only records it, asks you, or restores the newer version itself when
it is certain.

## 5. Where backups go

Settings → **Where backups go** (also: the *Change…* button in Backup & Sync).

| Mode | Who holds the history | The PS5 deletes its copy when |
|---|---|---|
| **PS5 only** | The console: old versions thinned and deduplicated there | Thinning (schedule) |
| **PC** | A PC client | The PC confirmed it (then after *Keep PC-acknowledged archives*, default 168 h) |
| **VPS** | Your vault; the PS5 uploads, encrypted | The vault confirmed it |
| **PC and VPS** | Both | Both confirmed it |

For **PC and VPS** you choose who uploads to the VPS:

- **This PS5**: two independent paths; the VPS is kept up to date even while
  the PC is off. Needs internet access on the PS5.
- **The PC**: the PS5 hands the PC client the vault address, token and key;
  the PC uploads and reports each upload back, so the PS5 keeps a backup until
  both the PC and the vault have it.

Switching modes never deletes anything already on a PC or in a vault. A PC
client that is not a destination of the current mode gets nothing.

The header shows one chip per destination (or *Backups on the PS5*). Each
opens a panel with status, actions and setup steps. Backup & Sync shows
*Waiting for PC / VPS* and *Delivered* (how many of all backups each
destination has).

## 6. Restoring

**Browse → a game** shows its saves with their state and last backup. Click a
save (or its ⋯ menu) for its versions:

- **Restore** a version of one save. A rollback copy of the current save is
  made first, so a restore can be undone.
- **Restore to an earlier point** (game menu): the whole game as it was at a
  restore point, including removing saves that did not exist then. A preview
  lists what will change.
- **Deleted saves** of the game can be brought back from their last version.
- **Delete save**: backs the save up first, then removes it.
- **Get from PC / Get from VPS**: when a version's backup is no longer on the
  PS5, Shallot asks the PC client (or the vault) for it, checks its SHA-256,
  then restores as usual.

Restores run through one pipeline: the target save is verified after writing
(container hash), and failures leave the old save untouched.

## 7. The PC client

`shallot_sync.py` (Python 3.8+, standard library; extras below). The PS5
serves it and its config: PC chip → *Set up a PC client*.

### Tray app (Windows)

1. Install Python 3 (tick *Add python.exe to PATH*).
2. Save `shallot_sync.py` and `config.json` from the PC panel into a folder.
3. `py -m pip install pystray pillow` (add `cryptography` for a VPS copy).
4. `py shallot_sync.py --autostart on` (starts it now and at every login).

### Docker (NAS, server, any OS)

1. Save `shallot_sync.py`, `Dockerfile`, `docker-entrypoint.sh` and
   `docker-compose.yml` from the PC panel into one folder (the compose file
   already holds the PS5 address and, with the login on, the client key).
2. `docker compose up -d`. Backups go to `./backups`, settings to `./config`.
3. On Linux, set `PUID`/`PGID` in a `.env` file if `./backups` is not
   writable. Update by downloading the files again and
   `docker compose up -d --build`; stop for good with `docker compose down`.

### How it collects

The client keeps one request open to the PS5 (`/api/backup/wait`); the PS5
answers it the moment a backup is queued or a command arrives, otherwise every
25 s. Downloads are written to a temporary file, checked against the SHA-256
and only then confirmed to the PS5. A backlog built up while the PC was off is
collected when it is back.

### Several clients: primary, stand-in, standby

Each client has an id (kept in its backup folder). The first client that
connects becomes the **primary**: the PS5 keeps every backup until the
primary has it, so the primary always holds the full history. Other clients
**stand by** and get nothing. If the primary is away for 30 minutes, one
connected client becomes the **stand-in** and collects copies meanwhile; the
PS5 still keeps everything for the primary, which collects what it missed when
it returns. PC panel → *PC clients*: **Make primary** (what a stand-in holds
counts as delivered), **Block**, **Unblock**, **Forget**.

### Managing it from the web UI

PC panel buttons (the client reports what it supports): **Sync now**,
**Ping**, **Pause / Resume** collecting (kept across restarts), **Thin out
now**, **Send to the VPS now**, **Update client** (tray: a Yes/No window on
the PC with the new script's SHA-256; terminal and Docker: only with
`"allow_remote_update": true`), **Quit client** (tray and terminal).

### On the PC

```
<backup_dir>/                 (default ShallotBackups next to the script)
  manifest.json               what this PC holds, upload records, client id
  <TITLE>/<save>/<id>.zip     backups still kept as ZIPs
  <TITLE>/_snapshots/*.json   restore points
  _blocks/                    deduplicated older versions (blocks.db + packs)
  _trash/                     thinned files, deleted after trash_days
  _vault/<id>.ids             block lists of archives uploaded to the vault
```

The PC thins its own history (`retention` below) every `interval_h` hours.

Command line (beside the tray/loop): `--once` (collect once and exit),
`--list TITLE`, `--restore ID_OR_ZIP [--save NAME] [--user ID]`,
`--export ID ZIP`, `--prune [--dry-run]`, `--dedup`, `--vault-pull` (rebuild
this PC's store from the vault), `--vault-gc` (have the vault delete unused
blocks), `--autostart on|off`, `--tray`, `-v`.

### config.json

| Key | Default | Meaning |
|---|---|---|
| `ps5_host`, `port` | — , 8082 | Where the PS5 is |
| `backup_dir` | `ShallotBackups` | Where backups go (relative to the config) |
| `client_key` | — | Needed when the web UI login is on |
| `allow_remote_update` | false | Let the web UI update a terminal/Docker client |
| `verify_downloads` | true | Check size and SHA-256 of every download |
| `poll_interval_s`, `http_timeout_s` | 30, 60 | For older PS5 builds / slow links |
| `log_file` | `shallot_sync.log` | |
| `retention.enabled` | true | Thin this PC's history |
| `retention.keep_all_days`, `daily_days`, `weekly` | 2, 30, true | The schedule |
| `retention.trash_days`, `interval_h` | 14, 6 | |
| `dedup.enabled`, `block_size`, `pack_mb` | true, 4096, 256 | Block store for older versions |
| `vault.url`, `token`, `key_file` | — | This PC's own vault (otherwise the PS5's settings in *PC and VPS*) |
| `vault.interval_min`, `slice_s` | 10, 20 | Upload pacing |

Environment variables override config.json (used by Docker):
`SHALLOT_PS5_HOST`, `SHALLOT_PS5_PORT`, `SHALLOT_BACKUP_DIR`,
`SHALLOT_LOG_FILE`, `SHALLOT_CLIENT_KEY`, `SHALLOT_CLIENT_NAME`,
`SHALLOT_VAULT_URL`, `SHALLOT_VAULT_TOKEN`, `SHALLOT_VAULT_KEY_FILE`.

## 8. The VPS vault

`vault/shallot_vault.py` (standard library), shipped as a Docker image. It
stores only encrypted, content-addressed blocks and encrypted descriptions;
it never sees a save or a key.

### Setting one up

1. On any machine with Docker that the PS5 or PC can reach (a small VPS; or
   your own PC to try it): save the files from the VPS panel (*Set up a
   vault*) into a folder.
2. Create `.env` with `SHALLOT_VAULT_TOKEN=<24+ random characters>` (the
   panel suggests one).
3. `docker compose up -d` and open TCP port **8091**.
4. Where backups go → VPS (or PC and VPS): `http://<vault address>:8091` and
   the token. *Test connection* in the VPS panel.

Optional HTTPS (bearer-token clients): set `VAULT_HOST` (a DNS name) and run
`docker compose --profile https up -d` (Caddy on 80/443 in front of port 8090).

### Security model

- Contents are encrypted with AES-256-GCM before they leave the PS5 or PC.
  Block ids and names are keyed HMACs. The key (`vault.key`, 32 bytes) never
  leaves your machines: the VPS panel's *Key* shows its fingerprint, reveals
  it to copy, or imports another (e.g. the PC's, so both write the same copy).
  **Keep a copy of the key elsewhere**: without it the vault cannot be read.
- Port 8091 is plain HTTP but accepts **signed requests only**: HMAC-SHA256
  of method, path, time, nonce and body hash with the token. The token never
  travels; replays and requests older than 5 minutes are refused.
- Clock: every answer carries `X-Shallot-Server-Time`; a client whose clock is
  off corrects the time it signs with and retries once.

### Keeping it small

- Identical blocks are stored once, across versions, games and clients.
- When the PS5 uploads, it thins the vault daily on the history schedule
  (Backup & Sync settings; *VPS: never thin* to keep everything), keeping
  versions a remaining restore point needs. With the PC uploading, the PC's
  retention applies.
- Each archive stores its block list (`meta/refs`, ids only); `POST /v1/gc`
  deletes blocks no list names and older than a day. It refuses while any
  archive lacks a list.

### Vault API (summary)

`GET /v1/ping` · `GET /v1/stats` · `POST /v1/blocks/missing|batch|fetch|delete`
· `PUT|GET /v1/blocks/<id>` · `PUT|GET|DELETE /v1/meta/<kind>/<name>` (kinds:
archive, snapshot, state, refs) · `GET /v1/meta/<kind>/` · `POST /v1/gc`.

## 9. The web UI

- **Header**: tabs, the destination chips (PC / VPS / Backups on the PS5),
  *Backups paused · Resume* when paused, *Stop Shallot*.
- **Browse**: games and saves; a game page with its saves, versions, restore
  points and deleted saves; the file view of a decrypted save (upstream
  Garlic browser: add, replace, export files).
- **Backup & Sync**: status (every row opens what it describes), diagnostics
  (*Scan Changed Saves Now*, catalogue checks), games, backup queue, recent
  events, settings. Tables have PC / VPS columns for the destinations in use;
  columns can be hidden (header right-click or ☰) and resized, per browser.
  Cards fold by their header. `?` buttons explain each action and setting.
- **Decrypt / Encrypt / Resign / Import**: the Garlic save tools. Drop files
  on the tile or click it.
- **Settings**: Where backups go, language, home screen, start with the
  console, web UI login, legacy manual cloud backups
  ([LEGACY_CLOUD.md](LEGACY_CLOUD.md)), About.
- The address keeps your place (`#sync/<TITLE>/objects`), so a reload returns
  to the same view. On the PS5 browser menus open as bottom sheets.
- Languages: English, Simplified Chinese.

## 10. Security

- **On your network the web UI is open by default**, as in Garlic: whoever
  reaches port 8082 can restore or delete saves and read the vault key.
- **Web UI login** (Settings): with a password set, every API request needs a
  login (cookie, 30 days, cleared by a Shallot restart), the **PC client key**
  (header `X-Shallot-Key`; included in the downloaded config.json and
  docker-compose.yml) or must come from the PS5 itself (the tile). The
  password is stored as a salt and 20000 rounds of HMAC-SHA256; failed logins
  are slowed down. A VPS never calls the PS5 and needs nothing.
- Updating the PC client from the web UI needs a Yes on the PC (tray) or an
  explicit setting, so nobody posing as the PS5 can push code to the PC.
- The LAN link between PS5 and PC is plain HTTP; contents are verified by
  SHA-256 at every hop.

## 11. Settings reference

`/data/shallot/sync.json` (Backup & Sync → Settings, and Settings → Where
backups go). Unknown keys are ignored; values are range-checked.

| Key | Default | Meaning |
|---|---|---|
| `enabled` | true | Automatic backups (Pause sets false) |
| `game_close_backup` | true | Back up when a game closes |
| `stability_check`, `stability_interval_s`, `stability_samples`, `stability_timeout_s` | true, 2, 2, 45 | Wait for saves to settle |
| `followup_rescan_s` | 180 | Re-check after close (0 = off) |
| `idle_sweep`, `idle_delay_min` | true, 10 | Idle scan |
| `startup_reconcile`, `startup_protect_untracked` | true, true | Checks at start |
| `regression_recovery` | ask | off / ask / certain |
| `rollback_before_restore` | true | Rollback copy before a restore |
| `max_queue_mb` | 2048 | Warn when backups kept on the PS5 exceed this (never deletes unconfirmed ones) |
| `keep_acked_hours` | 168 | Keep confirmed backups on the PS5 this long |
| `storage_mode`, `vps_route` | pc, ps5 | ps5 / pc / vps / both; who uploads in both |
| `vault_url`, `vault_token` | — | The vault's signed port and token |
| `ps5_keep_all_days`, `ps5_daily_days`, `ps5_weekly` | 2, 30, true | History schedule (PS5 only; the vault when the PS5 uploads) |
| `vault_keep_all` | false | Never thin the vault |
| `home_tile` | true | Add the home-screen tile at start |
| `system_probe`, `power_last_chance` | true, true | Rest/power-off handling |
| `pc_active`, `pc_blocked` | — | PC clients (managed in the PC panel) |
| `ui_pass_salt`, `ui_pass_hash`, `client_key` | — | Web UI login (managed in Settings) |

`bidirectional` is accepted but has no effect.

## 12. Files and paths

On the PS5:

| Path | Content |
|---|---|
| `/data/shallot/sync.json` | Settings |
| `/data/shallot/state/shallot_state.db` | State database (SQLite; versions, restore points, queue, events) |
| `/data/shallot/state/instance.lock` | One Shallot at a time |
| `/data/shallot/queue/` | Backups waiting for destinations; packs of deduplicated versions |
| `/data/shallot/vault.key` | Vault key (VPS modes) |
| `/data/shallot/shallot-savemgr.elf` | Stored copy for start with the console |
| `/data/shallot/cloud.json` | Legacy manual cloud backups |
| `/user/app/SHAL00001/` | Home-screen tile |

The database migrates itself forward (schema 12); an older Shallot cannot
read a newer database.

## 13. HTTP API

All endpoints are under `http://<ps5>:8082/api/` and return JSON. With the
login on, see [Security](#10-security).

**Status and settings**: `GET backup/status` · `GET|POST settings/sync` ·
`GET backup/events[?limit=]` · `GET system/probe` · `GET|POST system/launcher` ·
`GET|POST system/autostart?action=upload|on|off`.

**Games, saves, versions**: `GET backup/titles` · `GET backup/objects?title=` ·
`GET backup/saves[?title=]` · `GET backup/history?title=[&save=]` ·
`GET backup/snapshots?title=|since=` · `GET backup/compare?title=` ·
`POST backup/label?title=&object=&label=` · `GET backup/game_icon?title=` ·
`GET backup/diag/title?id=`.

**Backing up**: `POST backup/scan_changed` · `POST backup/protect_title?id=` ·
`POST backup/protect_object?...` · `POST backup/delete_object?...` ·
`POST backup/housekeeping`.

**Restoring**: `POST restore/from_queue?id=` · `POST restore/snapshot?id=[&object=][&exact=1&remove_fp=]` ·
`GET restore/snapshot_preview?id=` · `POST restore/upload?...` (body: ZIP) ·
`GET restore/status` · `GET recovery` · `POST recovery/resolve?...`.

**Checks**: `POST reconcile/title/sanity?id=` · `POST reconcile/title/full?id=[&protect=1]` ·
`POST reconcile/catalogue/quick` · `POST reconcile/catalogue/full[?protect=1]` ·
`GET reconcile/status|result|catalogue/result` · `POST reconcile/cancel`.

**Queue and PC client**: `GET backup/queue[?pending=1]` ·
`GET backup/queue_download?id=` (header `X-Shallot-SHA256`) ·
`POST backup/queue_ack?id=&dest=pc&sha256=` · `GET backup/wait?seq=&timeout=` ·
`POST backup/kick` · `POST backup/pc_manifest?title=` · `GET pc/status` ·
`GET|POST pc/clients[?id=&do=activate|block|unblock|forget]` ·
`POST pc/command?do=` · `POST pc/command_done?id=&ok=&result=` ·
`GET pc/vault_config` · `POST pc/vault_acks` · `POST pc/fetch?snap=[&object=]` ·
`GET pc/fetch_status?id=` · `GET pc/requests` · `POST pc/readopt?id=` (body: ZIP).
Downloads: `GET pc/shallot_sync.py`, `pc/config.json`, `pc/docker/<file>`,
`pc/vault/<file>`.

**Vault (PS5 side)**: `GET vault/status` · `POST vault/test` · `POST vault/sync` ·
`GET vault/key[?create=1|reveal=1]` · `POST vault/key` (body: 64 hex).

**Login**: `GET auth/status` · `POST auth/login|logout|password` ·
`GET|POST auth/client_key`.

PC clients identify themselves with `cid=<id>` on every request (older ones
by address).

## 14. Troubleshooting

| Symptom | Check |
|---|---|
| Page does not load after rest mode | Wait a few seconds: the listening socket is re-created on wake. |
| *the backup queue is already owned by another Shallot process* | A second copy was started; Quit first, then send the ELF. |
| PC chip: *PC offline · n waiting* | Is the client running? Can the PC reach `http://<ps5>:8082`? Backups wait safely on the PS5. |
| PC client: *standing by* | Another client is primary: PC panel → PC clients → Make primary. |
| PC client log: *web UI login is on* | Add `client_key` (Settings → Web UI login) to its config. |
| VPS: failing | VPS panel → Test connection; firewall port 8091; token; for the PS5 route, internet access on the PS5 (shared connections may not route). Clock differences are corrected automatically. |
| Docker: `Dockerfile` not found | The browser saved it as `Dockerfile.txt` (older Shallot builds); rename it. |
| A save not backed up | The game may be running; Backup & Sync → events (e.g. *still changing*); *Scan Changed Saves Now*. |
| Lost the vault key | The vault copy cannot be decrypted; keep a copy of the key elsewhere. |

## 15. Development

- **Build**: needs the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk):
  `make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk`, or from Windows through WSL
  with `tools/wsl-build.sh` → `shallot-savemgr.elf`. The Makefile embeds the web UI, the PC client and the
  Docker/vault files into the ELF.
- **Code**: `src/main.c` (Garlic: HTTP server, save mounting, tools) and
  `src/lansync_*.inc.c` (core/state DB, save scanning, worker, blocks, sync,
  reconcile, restore, recovery, system probe, PC link, launcher, HTTP routes,
  vault, crypto, auth); `src/ui.html` and `src/i18n/`; `pc/`; `vault/`.
- **Tests**: `tests/host/run.sh` (23 C suites in WSL); `tests/host/vault_e2e.sh`
  (the PS5 code against a real vault); `python -m unittest discover -s tests/pc`
  (PC client and vault); `tests/docker/docker_e2e.py` (both images);
  `tests/ui/mock_server.py` (the web UI with mock data, port 8099).
- **Relation to Garlic**: Shallot is generated from the Garlic LAN sync branch
  by `tools/rebrand_shallot.py` (names, `/data/shallot`, tile `SHAL00001`,
  credits). Design notes: `docs/lan-sync/`.

## 16. Testing status

Tested on a PS5 (firmware 13.60) by the author:

- automatic backups on game close and stability waiting;
- restoring one save and a whole game (exact, including removals), deleting
  and bringing back saves;
- rest-mode wake and power-off handling, page available again after wake;
- live PC sync (tray client), the home-screen tile (media category), start
  with the console, Pause/Quit, game icons.

Tested on a PC (host builds, real vault, Docker) but not yet with a console:
VPS uploads from the PS5 over the internet, the four storage modes switched on
a console, the PC client list (primary / stand-in), PS5-only deduplication on
large saves, the web UI login on the PS5, PS5 notifications.
