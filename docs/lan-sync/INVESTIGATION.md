# LAN Save Sync — Investigation Report (Stage 0)

Date: 2026-10-07. Base: upstream `c1409a1` (tag v1.13 / v1.13.1). Target firmware: 13.60.
Nothing in this report has been tested on a PS5. "Verified" below means verified by reading code/history only.

## 1. Upstream commit / version

- `upstream/main` = `c1409a1` "bump version to v1.13" (tags `v1.13`, `v1.13.1`). Makefile `VERSION ?= 1.13`.
- Repo is tiny: `src/main.c` (8109 lines, single TU, CRLF), `src/ui.html` (3708 lines, embedded via `xxd -i` → `src/ui.h`), `src/i18n/zh-CN.js`, `src/sqlite3.c` (amalgamation), `Makefile`, CI workflow.
- **No LICENSE file** in upstream or donor. Redistribution terms are unclear → keep our fork private or ask earthonion before publishing a public fork/binary.

## 2. SDK / toolchain (13.60)

- `ps5-payload-sdk` (github.com/ps5-payload-dev/sdk). CI downloads `releases/latest` (unpinned) and builds with host LLVM 18 (`clang-18 llvm-18 lld-18`) through `prospero-clang`.
- Latest SDK HEAD seen: `f7fd02e` (2026-09-23, "add support for llvm-23").
- Garlic contains **no firmware-offset code of its own**. Firmware coupling lives in the SDK's `libkernel_sys` (`kernel_set_ucred_authid`, `kernel_get/set_ucred_caps`) and in the user's ELF loader. 13.60 support therefore = SDK + loader; we must not touch these calls.
- Links: `-lkernel_sys -lkernel -lSceSystemService -lSceUserService -lSceFsInternalForVsh -lSceRegMgr -lSceNet`.
- Local machine: no make/clang/SDK on Windows; WSL `archlinux` exists (stopped). Building requires installing the SDK + LLVM in WSL (needs user approval) or relying on Forgejo/GitHub CI of our own fork.
- Recommendation: pin the SDK version we build against in `CLAUDE_PROGRESS.md` (record the release tag used for every build).

## 3. Existing automatic / cloud functionality

Already merged upstream (PR #4 `41e54f1`, PR #5 `08b595d`):
- `/data/garlic/cloud.json` config (`enabled`, `provider`, `watch_titles`, `ftp`, `auto_backup{enabled,poll_seconds,settle_seconds,queue_for_browser_drive}`, `backup_targets{local,usb,ftp,google_drive}`, `google_drive`).
- One staged ZIP per job in `/data/garlic/tmp` (`.tmp` → fsync → rename), distributed to local/USB (copy `.tmp`→rename), FTP (PS5 push), Drive browser queue (`/data/garlic/cloud_queue/*.zip`).
- ZIPs are **stored (no compression)** and contain the **raw encrypted save container** (+ PS4 `.bin` key). Packaging does **not mount** the save — it reads the image file under `/user/home/<uid>/savedata_prospero/<TID>/<save>` directly. Good for safety: backup = file read only.

## 4. Existing queue / API behaviour

- `GET /api/cloud/queue` → readdir of `cloud_queue/*.zip` (id = filename sans `.zip`, size, mtime). No hash, no metadata, no destination state.
- `GET /api/cloud/queue_download?id=` → streams file with Content-Length.
- `POST /api/cloud/queue_done?id=` → `unlink`. Single implicit consumer (browser Drive uploader).
- Queue only fed when: auto job **and** title job **and** Drive enabled **and** browser mode **and** `queue_for_browser_drive`.
- Single global job slot (`g_cloud_state.running`); a second request fails with "Cloud upload already running".

**Safety gaps found (upstream):**
1. `auto_on_title_closed` stores the new signature **before** the backup runs; if the job fails or is rejected ("already running") the change is forgotten forever.
2. `copy_file()` ignores `write()` errors and does not fsync → a short copy can be renamed into a valid-looking queue `.zip`.
3. No startup cleanup of `.tmp` files; orphaned stage files in `/data/garlic/tmp` after a crash.
4. Snapshots are RAM-only (lost on payload restart / panic); table capped at 64 titles.
5. Fixed `sleep(settle_seconds)` (default 15 s) — no stability test.
6. Watcher sleeps inline during settle, so it cannot see a new title start during that window.

## 5. Running-title detection

`auto_watcher_thread` polls every `poll_seconds` (default 5): `sceSystemServiceGetAppIdOfRunningBigApp()` + `sceLncUtilGetAppTitleId()`. Title change / app-id change / no app ⇒ "closed". Only "Big App" (games) are seen. Reusable as-is.

## 6. Save change detection

`auto_title_signature()` = FNV-1a over (save_name, user_id, st_size, st_mtime) for all non-USB saves of the title. Per-title, second-resolution mtime, size is mostly constant (PFS images are pre-allocated) ⇒ mtime is effectively the only signal. Additional cheap signals available: `st_mtim` nanoseconds, `st_ctime`, `st_blocks`, `savedata.db` row `mtime` (`/system_data/savedata_prospero/<uid>/db/user/savedata.db`, table `savedata`, columns incl. `title_id, dir_name, mtime`). Strong signal: SHA-256 of the container (no SHA-256 in tree yet; need to add a small one).

## 7. Restore / import implementation

Browser-driven, multi-request, inline in `handle_request_inner`:
- `POST /api/import_encrypted` → writes upload to `/data/save_files/_tmp_import`, `mount_by_path()` at `/data/mount_sd`, parses `sce_sys/param.sfo`, checks account-id match + existing save.
- `GET /api/import_finish` → creates/places the container under `savedata_prospero`, updates `savedata.db` (UPDATE/INSERT), handles zeroed SFO regeneration.
- One global mount (`g_mounted`, `/data/mount_sd`), protected by `g_mtx`. No rollback copy. To reuse it server-side for PC→PS5 restore, the import logic must be **extracted** from the HTTP handler into functions (careful, behaviour-preserving refactor; highest-risk area of the project).

## 8. Suspend / rest / shutdown / reboot APIs in current SDK

Stubs exist in `libSceSystemService` (already linked); **no headers, no samples** — prototypes must be declared by us and signatures are unverified:
- Observe: `sceSystemStateMgrGetCurrentState`, `sceSystemStateMgrIsShellUIShutdownInProgress`, `sceSystemStateMgrIsStandbyModeEnabled`, `sceSystemServiceReceiveEvent`, `sceSystemServiceGetEventForDaemon`, `sceSystemServiceEnableSuspendNotification`, `sceSystemServiceDeclareReadyForSuspend`, `sceSystemServiceIsAppSuspended`.
- Act: `sceSystemStateMgrEnterStandby`, `sceSystemStateMgrTurnOff`, `sceSystemStateMgrReboot`, `sceSystemServiceRequestPowerOff`, `sceSystemServiceRequestReboot`, `sceSystemStateMgr{Start,Extend,Cancel}ShutdownTimer`.

## 9. Are normal transitions observable early enough?

**Unknown — must be probed on the console.** Event APIs (`ReceiveEvent`, suspend notification) are designed for registered applications/daemons; an elfldr payload is neither, so they may never fire. Polling `GetCurrentState` / `IsShellUIShutdownInProgress` may work but the time budget before the payload is frozen is unknown. Plan: Stage O ships a **diagnostic probe** (log-only, no backup) first. Until proven, normal Rest/Shutdown handling is **best-effort**, and Safe Rest/Shutdown/Reboot (Garlic does the backup, then calls the transition API) is the reliable path — itself **unverified** until tested.

## 10. Is running-title save access safe?

Not demonstrably. The game may be mid-write to the container; a raw read can capture a torn image, and mounting a live save risks interfering. ⇒ No live backups by default (matches requirements). Backups only after exit + stability test, or while idle.

## 11. Which donor (cloud-save-backup) changes exist upstream?

All of them. `cloud-donor/cloud-save-backup` (`8a659d9`), `cloud-donor/multi-backup-targets` (`50a1fae`) and `cloud-donor/main` (`570dff6`) are all ancestors of `upstream/main`. Nothing to cherry-pick; donor remains reference only. Upstream changes after the merge: PFS chmod 0777 fixes (v1.11/v1.12), zh-CN i18n.

## 12. Proposed state machine

Per save key `(user_id, title_id, save_name)`:

```
OBSERVED(fp_meta) ──change──► CHANGED ──game running──► DEFERRED
     ▲                           │ idle / closed
     │                           ▼
     │                      SETTLING (stability samples)
     │                     ┌─────┴──────┐
     │               stable│            │timeout
     │                     ▼            ▼
     │                 PACKAGING     UNSTABLE (retry at idle; never marked protected)
     │                     │ ok (.tmp→fsync→rename, sha256)
     │                     ▼
     └──────────── LOCAL_QUEUE_PROTECTED (gen N)
                           │ PC GET + ack(sha256)
                           ▼
                     LAN_PROTECTED ──(optional)──► CLOUD_PROTECTED
```
Destination states are independent columns, never one boolean.

Watcher (title lifecycle): `IDLE → RUNNING(tid) → EXITED(tid) → SETTLING → BACKUP → IDLE`. A second game start during SETTLING aborts settling for the old title (it is retried at idle) and never touches the new title.

Jobs run on **one serialized save-operation worker** with a priority queue (game-close > system-transition > idle sweep > reconcile/protect > manual), replacing "already running → fail". Read-only cheap checks bypass the worker.

## 13. Reconciliation workload model

| Level | Op | Scope | Work | Writes |
|---|---|---|---|---|
| 1 | Sanity Check Game | 1 title | stat + savedata.db + state DB + queue files | none |
| 2 | Quick Catalogue Check | all | same, metadata only | none (stores result) |
| 3 | Full Reconcile Game | 1 title | + SHA-256 where meta changed/ambiguous, + PC manifest if reachable | Garlic bookkeeping only |
| 4 | Full Catalogue Reconciliation | all | Level 3 per title, async job, progress, cancel between saves | bookkeeping only |
| 5 | Full Catalogue + Protect | all | Level 4 then enqueue backups for UNBACKED_CHANGE only | archives |

Jobs are asynchronous (`POST` starts → `GET /api/reconcile/status` polls). One heavyweight job at a time; restore operations take the same exclusive lock. Running title is skipped/deferred.

## 14. Minimal files requiring modification

- `src/main.c` — new sections added as clearly delimited blocks (state DB, SHA-256, stability, worker, queue v2, reconcile, sync, system-state); targeted edits in the watcher, `copy_file`, queue endpoints, `main()`.
- Optionally split new code into `src/backup_*.c/h` to keep the diff reviewable (Makefile change limited to adding sources — no toolchain changes).
- `src/ui.html` — new "Automatic Backup / Sync" and "Reconciliation / Diagnostics" sections; i18n keys.
- `README.md` — new docs section.
- New: `pc/garlic_sync.py` (+ `pc/config.example.json`), `docs/lan-sync/*`, `CLAUDE.md`, `CLAUDE_PROGRESS.md`.

## 15. Compatibility risks

- SDK drift (CI uses `latest`): new SDK may change libc/headers. Mitigation: record SDK tag per build.
- `SQLITE_THREADSAFE=0`: the bundled SQLite must be used from one thread at a time → all state-DB access goes through our own mutex.
- Extracting import/restore logic from the giant handler could regress manual import → do it last (Stage M) and keep the HTTP handlers calling the same functions.
- Undeclared system-state API signatures → wrong prototype can crash the payload; guard behind config flag, probe-only first.
- Browser Drive uploader relies on `queue_done` deleting the file → keep semantics: `queue_done` = "cloud ack"; file deleted only when retention allows.
- Memory: PS5 payload heap is fine for SHA-256 streaming (512 KiB buffer already used).

## 16. Proposed persistent-state format

Use the **already-bundled SQLite** (`/data/garlic/state/garlic_state.db`, `journal_mode=DELETE`, `synchronous=FULL`) for atomic multi-row updates; config stays in JSON (`cloud.json` gains a `sync` object). Tables (draft):

- `saves(user_id, title_id, save_name, last_seen_meta, last_seen_at, last_hash, state, last_generation, missing_since, PRIMARY KEY(user_id,title_id,save_name))`
- `generations(gen_id INTEGER PK, user_id, title_id, save_name, sha256, size, meta, parent_gen, created_at, reason, archive_id)`
- `queue(archive_id PK, gen_id, filename, sha256, size, created_at, state{tmp,ready,interrupted}, local_ok, lan_ack_at, cloud_ack_at, ftp_ok, usb_ok, attempts, last_error)`
- `ops(op_id PK, kind, target, started_at, finished_at, status{running,ok,failed,interrupted}, detail)` — anything `running` at startup becomes `interrupted`.
- `reconcile(title_id PK, last_sanity_check, last_full_reconcile, last_catalogue_scan, last_result, last_known_current_fp, last_known_pc_generation, last_common_generation, recommended_action)`
- `pc_manifest(user_id,title_id,save_name,gen_id,sha256,acked_at)` — what the PC confirmed it holds.
- `meta(key PK, value)` — schema version, last startup reconcile, last idle scan, etc.

Fingerprints: `meta` fp = (size, mtime_ns, ctime_ns, blocks, savedata.db mtime); `strong` fp = SHA-256 of container (+ `.bin` for PS4). Generation ancestry = `parent_gen` chain per save on this console; PC-side generations are identified by SHA-256.

Queue files: `/data/garlic/queue/<archive_id>.zip` (+ `.tmp` while writing). `cloud_queue/` kept as a compatibility view (existing files imported on first start).

## 17. Proposed PC-client changes

No existing PC client talks to Garlic's HTTP API (`garlic-cli` targets garlicsaves.com; `garlic-worker` is a console worker). Proposal: new **stdlib-only Python 3** client `pc/garlic_sync.py` (Windows-first, no internet):
- poll `GET /api/backup/queue` → `GET /api/backup/queue_download?id=` → write `.part` → verify Content-Length + `X-Garlic-SHA256` → fsync → atomic rename → `POST /api/backup/queue_ack?id=&dest=pc&sha256=`.
- Layout `<root>/<TITLE>/<SAVE>/<TITLE>_<SAVE>_<timestamp>_<gen>.zip`, never overwritten; local `manifest.json` (atomic write).
- `config.json` (gitignored) with address, backup dir, poll interval, verify, retention, sync mode; Task Scheduler autostart instructions; later: manifest exchange + restore push.

## 18. Resume / checkpoint strategy

- `CLAUDE.md` (auto-loaded by Claude Code): git/safety rules + "read CLAUDE_PROGRESS.md first".
- `CLAUDE_PROGRESS.md`: git state, stage checklist with status vocabulary, build info, verified/unverified assumptions, next task.
- `docs/lan-sync/REQUIREMENTS.md`: the full original requirements (source of truth for scope).
- Small commits per stage; push to `origin` (once the user supplies it) after each checkpoint.
