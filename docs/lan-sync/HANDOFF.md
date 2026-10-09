# Handoff — Garlic LAN sync fork and Shallot (2026-10-08)

Read with `CLAUDE.md` (rules) and `CLAUDE_PROGRESS.md` (stage table R1–R13).
Nothing here is pushed; pushing is the owner's job.

## Branches and folders
| | Folder | Branch | ELF |
|---|---|---|---|
| Garlic (for a pull request to upstream) | `GarlicProject/garlic-savemgr` | `feature/lan-save-sync` | `garlic-savemgr.elf` |
| Shallot (owner's own fork, GitHub `dxalp3`) | `GarlicProject/shallot-savemgr` (git worktree) | `shallot` | `shallot-savemgr.elf` |

Shallot = feature branch + `tools/rebrand_shallot.py` (names, `/data/shallot`,
tile `SHAL00001`, credits to Garlic SaveMgr by earthonion, icon, vault test
vectors). Work happens on `feature/lan-save-sync`; then in the Shallot folder:

    git merge feature/lan-save-sync
    # conflicts on lines that mention Garlic: take the feature side, then rebrand
    git checkout --theirs <file>    (for each unmerged file; git add them)
    py -3 tools/rebrand_shallot.py  (refuses to run while conflicts remain)
    git add -A && git commit

README/CHANGELOG are untouched on purpose (owner will write one per fork).
Upstream has no LICENSE: the owner should get earthonion's permission before
publishing Shallot.

## Build and test (Windows host, WSL archlinux)
- ELF: `wsl.exe -d archlinux -- sh -c "sed 's/\r$//' <repo>/tools/wsl-build.sh | sh -s <repo>"`
  (`<repo>` as `/mnt/c/...`). Run it as its own command: it clears a fixed
  build folder, and the Claude Code safety check blocks it when chained.
- Host C tests: `wsl -e sh -c "cd <repo> && sh tests/host/run.sh"` → 23 suites.
  For Shallot set `TMPDIR=/tmp/shallot-ht`.
- PC tests: `py -3 -m unittest discover -s tests/pc` → 60 tests (needs `cryptography`).
- Docker e2e: Docker Desktop CLI at
  `C:\Users\s4cha\AppData\Local\Programs\DockerDesktop\resources\bin` (not on PATH);
  `py -3 tests/docker/docker_e2e.py` builds both images, stand-in PS5, real vault.
- Vault e2e from the PS5 C code: `tests/host/vault_e2e.sh` (KEEP_VAULT=1 +
  `tests/pc/vault_crosscheck.py`) — last run before R11, not re-run since.
- UI mock: `.claude/launch.json` in `GarlicProject` has `garlic-ui-mock`
  (port 8099) and `shallot-ui-mock` (8100). Restart it after changing
  `tests/ui/mock_server.py`. The owner often uses the same preview tab: test
  in a separate background tab. A hash-only navigation does not reload.

## Where things stand (all TESTED ON HOST / mock, NOT on the PS5 unless noted)
- Destinations: `storage_mode` pc | ps5 | vps | both (+ `vps_route` ps5 | pc).
  An archive leaves the PS5 only when every destination has it; in "both"
  with the PC uploading, the PC reports vault uploads (`/api/pc/vault_acks`).
- A PC client that is not a destination (PS5 only, VPS only) gets nothing.
- PC clients: clients send `cid`; the first one becomes the primary
  (`sync.json pc_active`), the PS5 forgets an archive only after the primary
  has it, so the primary always ends up with the whole history. If the
  primary is away for 30 min (`LS_PC_LEASE_S`), one connected client stands
  in: it collects copies (queue column `standin_ack`, schema v11) without
  letting the PS5 delete anything; the primary collects what it missed when
  back. "Make primary" on the stand-in counts its copies as delivered.
  `/api/pc/clients` to switch, block, forget. A non-Docker 2.2 client takes
  over an address-only (pre-2.2) primary of the same machine (needs a real
  peer address; not covered by tests).
- Optional web UI login (`src/lansync_auth.inc.c`, Settings > Web UI login):
  every /api/ request needs a session cookie, the PC client key
  (`X-Garlic-Key`, sync.json `client_key`) or to come from 127.0.0.1 (the
  PS5's own browser). Password = salt + 20000 x HMAC-SHA256; sessions in
  memory (30 days). Downloaded config.json / docker-compose.yml carry the key.
  A VPS never calls the PS5, so it needs nothing.
- sync.json writes hold `g_ls_cfg_mtx` (settings, PC client changes, login,
  home tile); reads are lock-free (atomic rename).
- PC client 2.3: sends the client key; "update" from the web UI needs a Yes
  on the PC (tray on Windows asks, with the new script's SHA-256) unless
  config.json has `"allow_remote_update": true`.
- PC client 2.2 (tray, terminal or Docker): commands from the web UI (ping,
  sync, pause/resume, thin, vault, update, quit), signed vault requests,
  relay settings from the PS5, standby, vault-upload reports.
- Docker: `pc/` image (su-exec, PUID/PGID), `vault/` image (signed port 8091;
  Caddy only with `--profile https`). The PS5 serves all these files.
- UI: picker in Settings, PC and VPS chips and panels, PC clients list,
  tables with per-destination columns (hide/resize), folding cards, `?` help,
  reload keeps `#tab/game/view`, legacy cloud section, zh-CN complete.
- The ✕ on a Backup & Sync detail steps up one level (`lsDetailBack`): a
  save's versions → that game's saves → closed. Chosen over "the view before"
  because the game's view buttons make that history unpredictable. Browse's
  game page has no ✕ of its own (it uses Back); extend `lsDetailBack` if new
  nested views are added (e.g. a snapshot's contents → snapshots).
- Owner-verified on the PS5 earlier: stages O/P1, wake socket re-creation,
  tile install (before the media-category fix), R1 live sync (72 archives).

## Owner to test on the PS5 next
- Done by the owner (2026-10-08): the home-screen tile (media category),
  Pause vs Quit, start with the console, game icons.
1. (open) The four storage modes, the PC client list (primary / stand-in), and
   PS5-only thinning with deduplication on large real saves (speed included).
2. Where backups go: each mode; PC chip/VPS chip states; queue PC/VPS columns.
3. PC clients: run the 2.0 tray, then update it (web UI "Update client" only
   exists from 2.1 — replace `garlic_sync.py` by hand once); try Docker on the
   same PC and switch between them.
4. VPS: run the vault locally in Docker on the PC (`http://192.168.137.1:8091`)
   — tests everything except the internet path.
5. (open) VPS straight from the PS5 over the internet (through the PC's shared
   connection; DNS) — waits for a real VPS.
6. Web UI login: password hashing speed on the PS5, the tile still opening
   without a password.

## Vault housekeeping (2026-10-08)
- Clock: every vault answer carries `X-Garlic-Server-Time`; the PS5 and the PC
  keep the difference (`g_ls_vault_skew` / `VaultClient.skew`) and retry once
  after a 401 caused by the clock.
- Block lists: next to each archive meta, `meta/refs/<same name>` holds its
  block ids in the clear (keyed hashes). `POST /v1/gc` (vault v2) deletes blocks
  no refs list names, older than `min_age_s` (default a day); it refuses while
  any archive lacks a refs list. The PC back-fills refs from `_vault/*.ids`;
  `--vault-gc` and the client never delete blocks themselves any more (a PS5
  may share the vault).
- Thinning: when the PS5 uploads, `ls_vault_thin` (daily, in the vault job)
  applies the history schedule (`ps5_keep_all_days`, `ps5_daily_days`,
  `ps5_weekly`; off with `vault_keep_all`) to restore points and archives in
  the vault, keeps archives a remaining restore point needs, marks
  `vault_gone` (schema v12) and asks the vault to clean up. With the PC
  uploading, the PC's own retention applies (forget_dropped).
- PS5 notifications (`ls_health`, hourly, each at most daily): VPS failing
  over an hour, backups waiting for the PC over 3 days, queue over its limit.
- Backup & Sync status: Delivered — PC n of m, VPS n of m (all backups made).
- Docker images pinned: python:3.12.7-alpine3.20, cryptography 43.0.3.

## Open ideas (not started)
- README / CHANGELOG per fork (owner asked not to write them yet).
- Several PS5s feeding one PC client or vault (one console per client today).
- The Browse file view (upstream) keeps its own look; the tool tabs, Settings
  and Backup & Sync share the card/tile style.

## Gotchas for the next session
- Patch with Python scripts written via the Write tool; bash heredocs mangle
  backslashes. Working copies are LF (autocrlf); `src/main.c` 1-space indent.
- `git merge` into Shallot conflicts on UI/i18n lines often; never chain the
  rebrand + commit after a merge without checking `git diff --diff-filter=U`.
- The owner's GarlicSync folder still runs client 2.0.
