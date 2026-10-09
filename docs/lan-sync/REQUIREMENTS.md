# LAN Save Sync — Requirements (from project owner, 2026-10-07)

Source of truth for scope. Condensed from the owner's original brief; wording kept where it defines behaviour.

## Purpose
Turn Garlic SaveMgr into a primarily automatic save-protection, synchronization, recovery and reconciliation system for a jailbroken PS5 on **firmware 13.60**. Chief concern: **save loss** from kernel panics, failed Rest Mode, shutdown/reboot, save regression after a crash, queued backups lost on payload restart, slow save finalization, inconsistent state after interrupted sessions.

No aggressive live backups while playing if that risks game save I/O. Protect around lifecycle events: after game close; around Rest Mode / shutdown / reboot where technically safe; while idle; when Garlic (re)starts after a crash; on manual reconcile/validate from the web UI.

The **PC on direct Ethernet/LAN is the primary durable repository**. Cloud is optional/secondary.

Prefer: recoverability, integrity, clear state transitions, atomic operations, conservative restores, deterministic conflict handling, tiered diagnostics, minimal firmware-specific changes, resumable development — over clever but fragile automation. **Never trade the user's current save for sync convenience.** PS5→PC may be aggressive; PC→PS5 must be conservative.

## Baseline
Current upstream (earthonion) is the base. Do not downgrade/replace: Payload SDK, kernel compat code, linker setup, firmware/runtime detection, save mount logic, credential handling, ELF-loader compat, 13.60 support. StamatisD's cloud-save-backup branch is a donor/reference only. Avoid unrelated refactors.

## Architecture
Pull-based over Garlic's existing HTTP server; no FTP server needed on PC.
`save → Garlic decides backup → ONE encrypted/restorable archive → persistent backup queue → PC polls → downloads → verifies, stores durably → acknowledges → Garlic updates destination state/retention.`
Same archive may feed PC, optional Drive, optional legacy FTP, optional USB/local. Never package once per destination. Generalize "cloud queue" naming internally; reuse/keep backward compatibility with `GET /api/cloud/queue`, `GET /api/cloud/queue_download?id=`, `POST /api/cloud/queue_done?id=`.

## Automatic backup policy (priority)
1 game close, 2 rest/suspend, 3 shutdown/reboot, 4 idle sweep, 5 startup/crash-recovery reconciliation, 6 manual. Do not try to react to a kernel panic itself; maximize chance a stable save was already protected.

### 1. Game close (primary)
game starts → remember title → exits → identify relevant saves → wait until **demonstrably stable** → determine changed saves → package changed → enqueue. No fixed sleep only.

**Save stability test:** sample metadata, wait, resample, compare; package only after N consecutive matching observations. Signals: mtime, size, directory entry metadata, save DB metadata, file count, other cheap metadata — not mtime alone if better exists. Configurable: sample interval (default 2 s), stable observations (default 2), max settle timeout (default 30–60 s). On timeout: don't assume consistency, log/report, optionally retry at idle, never record as successful. Fast games back up almost immediately; slow games automatically get more time.

### 2. Rest mode / suspend
Investigate whether SDK/system APIs give an early enough pre-suspend notification for shell-triggered Rest Mode. If yes: running title → protect only that title if safe; no title → lightweight changed-save sweep. Never mount/copy/package a running title's save during an unsafe write window. Investigate: is the title quiescent at notification time, are containers still mounted, does app suspension precede payload suspension, is there enough time. If not reliable, don't claim it. Optionally implement **Safe Rest Mode** (user requests via Garlic → finish safe pending backup → stability → persist queue → flush/close → invoke Rest Mode). Label each path: verified safe / best-effort / unsupported.

### 3. Shutdown / reboot
Same philosophy; optionally **Safe Shutdown / Safe Reboot** (backup → stability → persist queue → flush → invoke transition) with a hard timeout; never delay indefinitely; log incomplete state accurately.

### 4. Idle sweep
No title running and idle for a configurable delay (default 5–15 min): lightweight scan of all saves using persistent per-save fingerprints — **not** repackaging everything. Cheap metadata first (title/save id, mtime, size, DB metadata); hash only where necessary. Changed → stability test if appropriate → package → enqueue → update protected fingerprint only after archive creation succeeds. Manual action: **Scan Changed Saves Now**.

### 5. Startup / crash recovery reconciliation
Garlic may vanish (panic, reboot, power loss, failed rest, payload crash, manual restart). On start: load persistent state; inspect queue; preserve all valid unacknowledged archives; safely clean obviously abandoned `.tmp`/`.part`; never delete a valid queued ZIP because of restart; recover queue indexes/metadata; keep exposing old queued archives to the PC; mark previously in-progress operations **interrupted**, not successful. Once stable/idle: lightweight scan; compare current vs last-known fingerprints; classify unchanged / newer surviving / regressed / missing / ambiguous; queue backups for surviving changes never protected; detect regression vs PC-acknowledged versions. Conservative: reconstruct state first, never auto-overwrite because fingerprints differ. If current A is a known older ancestor of PC-acked B → "Newer previously-synchronized PC backup available" (probable regression). Unknown current save → branch/conflict, not an automatic restore target.

### 6. Manual reconciliation / diagnostics (distinct workloads)
- **L1 Sanity Check Game** (cheap, one title, metadata only): enumerate known containers; confirm entries exist; compare cheap metadata with recorded; queue entries reference valid files; manifest state; missing/unexpected saves; interrupted ops; pending PC download/restore; regression markers. No packaging, no expensive hashing unless needed, no save modification. Results: HEALTHY, CHANGED_SINCE_LAST_SCAN, UNBACKED_CHANGE, PC_NEWER, PS5_NEWER, KNOWN_REGRESSION, CONFLICT, MISSING_SAVE, QUEUE_INCONSISTENCY, MANIFEST_INCONSISTENCY, UNKNOWN/NEEDS_FULL_SCAN.
- **L2 Quick Catalogue Check** (all titles, metadata only, avoid hashing/mounting) — e.g. "142 checked, 139 unchanged, 2 changed, 1 missing, 0 manifest errors"; click anomalous title → Sanity/Full Reconcile.
- **L3 Full Reconcile Game**: enumerate, full metadata, compare with manifest/history, strong hashes where useful, compare with PC generations if reachable, queue state, rollback/restore state, ancestry, missing/new/changed/regressed, duplicate/abandoned queue entries, whether current version lacks a safe backup, detailed report. Offer (not force) **Back Up Changed Saves**; separate op **Full Reconcile + Protect Changed Saves**. Pure reconcile is read-only except documented repair of Garlic's own stale bookkeeping. Never modify game save contents.
- **L4 Full Catalogue Reconciliation**: state audit of everything (enumerate, metadata, strong fp where needed, vs manifest/queue/PC history; orphaned queue files; manifest records whose save disappeared; saves not in history; regressions; PC-newer; conflicts; never-backed-up changes). Does **not** ZIP everything. Only when idle unless running title can be excluded. Progress (n/total, current title, changed, regressions, conflicts, unprotected). Cancellable between titles/saves; metadata stays consistent.
- **L5 Full Catalogue Reconciliation + Protect**: L4, then stability + package only genuinely changed/unprotected saves; skip unchanged; never restore automatically. Not the default.

**Reconciliation ≠ backup ≠ restore.** Reconcile = what state is everything in; backup = which versions need packaging; restore = should a known version replace current. Reconcile may recommend, never silently restore.

Per-title UI actions: Sanity Check, Full Reconcile, Back Up Changed Saves, Compare With PC, View History, Inspect Queue, Inspect Conflict/Recovery State; Restore From PC and Restore Previous Generation are separate explicit actions.

Store per title: last_sanity_check, last_full_reconcile, last_catalogue_scan, last_result, last_known_current_fingerprint, last_known_pc_generation, last_common_generation, recommended_action. Recommendations: No action needed / Backup recommended / Recovery available / Conflict / Full scan recommended.

Safety: no overwrites; don't delete unknown files; no auto restore on mismatch; avoid mounting running title's saves; no simultaneous full scans of same title; serialize full catalogue reconcile against restore; cheap read-only checks may run concurrently only if safe; cancellation points between items. If a game starts during a catalogue scan: finish/abort current non-destructive step, exclude/defer running title, continue only if safe.

API (follow Garlic conventions): `POST /api/reconcile/title/sanity`, `POST /api/reconcile/title/full`, `POST /api/reconcile/catalogue/quick`, `POST /api/reconcile/catalogue/full`, `GET /api/reconcile/status`, `GET /api/reconcile/result`, optional `POST /api/reconcile/protect_changed`. No expensive state-changing GETs. Long jobs async with status. One heavyweight catalogue job at a time.

## Change tracking
Persist across restarts per save: title id, save id, last observed fp, last packaged fp, last PC-acked fp, last cloud-acked fp, timestamp, archive id/path, generation/ancestry. Atomic metadata writes. Never mark protected merely because a scan saw it. Stages tracked independently: SAVE OBSERVED → ARCHIVE CREATED (LOCAL_QUEUE_PROTECTED) → PC ACKNOWLEDGED (LAN_PROTECTED) → CLOUD ACKNOWLEDGED (CLOUD_PROTECTED).

## PC / LAN transfer
PC pulls: GET queue → GET archive → write `.part` → verify size/hash (Content-Length, size, SHA-256 from Garlic) → fsync/close → atomic rename `.zip` → acknowledge. Ack only after durable write. Failure leaves PS5 entry untouched. Layout `<root>/<TITLE_ID>/<SAVE_ID>/<TITLE_ID>_<timestamp>.zip`; never overwrite history.

PC app: extend an existing client if one speaks Garlic's HTTP API (none found), else minimal new one. Settings: Automatic Save Backup, PS5 address, Backup Directory, Poll Interval, Start Automatically, Verify Downloads, Retention Policy, Bidirectional Sync, Optional Cloud Replication. Quietly drains queue. Direct Ethernet; no internet required.

## Bidirectional sync
Optional PC→PS5. Never overwrite a running title's save. PS5→PC automatic after safe checkpoints; PC→PS5 only while title inactive.
Manifest per save: title id, save id, content fp, archive timestamp, SHA-256, generation id, last common generation, PC ack state, PS5 current state. **Never decide by mtime alone.**
Cases: (1) PS5==PC → nothing. (2) PS5 changed, PC at common → backup. (3) PC changed, PS5 at common → allow restore while inactive. (4) both changed → CONFLICT, never auto-overwrite either.
On GAME_START: remember title, disable auto restore for it, if PC restore pending defer until close and report.
Preferred restore window: idle, manifests exchanged, stage PC-newer, validate, rollback backup, restore, verify, update metadata. Pre-launch interception: investigate only; implement only with a stable community-tested 13.60 mechanism; no fragile ShellCore hooks.

Restore safety: use Garlic's existing import/restore logic; never overwrite random save-system files. Steps: title not running → validate archive → verify title/save/account → rollback backup of current → stage → restore → verify → update manifest only after success. Staging `incoming.part → verify → incoming.ready → rollback → restore → verify → commit`. On failure keep rollback copy, incoming archive, failure log.

Conflict (common A; PS5 A→B; PC A→C) even if C is newer by time. Choices: Keep PS5 / Restore PC / Preserve both / Inspect metadata.

## Panic / regression recovery
Example: 18:00 A, 18:30 B, 18:31 B PC-acked, 18:45 panic, PS5 now A ⇒ PROBABLE SAVE REGRESSION; offer Restore B / Keep A / Inspect. Config Automatic Regression Recovery: Off / Ask (default) / Automatic only when ancestry is certain (title not running, archive valid, ids match, current is demonstrably an older known generation, no divergent unknown branch).
After panic: don't restore immediately; recover queue, reconstruct metadata, wait for idle, scan, compare with known generations, query PC manifest, classify (SYNCED, PS5_NEWER, PC_NEWER, KNOWN_REGRESSION, CONFLICT, UNKNOWN_CHANGED_STATE, MISSING_ON_PS5, MISSING_ON_PC), only then decide.

## Cloud
Secondary. Preferred: Garlic → PC → optional PC-side cloud copy. Existing Drive support may consume the same queued archive; never package twice; cloud failure never invalidates LAN success. Track LOCAL_QUEUE_PROTECTED, LAN_PROTECTED, CLOUD_PROTECTED separately.

## Queue safety
`.tmp` during creation; final ZIP only on success; `.part` during PC download; atomic rename; interrupted download doesn't delete source; interrupted creation never looks valid; no duplicate concurrent backup of same save; serialize auto/manual ops; preserve queue across restart; never silently drop an unacknowledged recent backup. Bounded retention: when full delete oldest PC-acknowledged copies first, then redundant acknowledged files; preserve newest unacknowledged backup, rollback data for pending restore, unresolved conflict data. Warn visibly when constrained.

## Running-game safety
Data safety > backup frequency. No aggressive mounting/packaging of running titles. A proven snapshot-safe running-title mechanism may be documented and optional, never default without strong evidence.

## Web UI
Automatic Backup / Sync section: automatic backups, game-close backup, stability checking, sample interval, stable samples, max wait, rest-mode protection, shutdown/reboot protection, idle sweep, idle delay, PC connection status, queue size, last successful backup, last PC ack, last startup reconciliation, last idle scan, last sync, last error.
Reconciliation / Diagnostics section — global: Quick Catalogue Check, Full Catalogue Reconciliation, Full Catalogue Reconciliation + Protect; per title: Sanity Check, Full Reconcile, Back Up Changed Saves, Compare With PC, View History, Inspect Queue, Inspect Conflict/Recovery State. Show expected workload (Quick / Moderate / Deep). Catalogue progress, title count, anomalies, changed, conflicts, regressions, errors. Also: Scan Changed Saves Now, Reconcile Now, Inspect Queue, Retry Pending Transfers, Safe Rest Mode, Safe Shutdown, Safe Reboot (only if verified).
Per title/save states: Synced, PS5 newer, PC newer, Conflict, Pending PC download, Pending PS5 restore, Restore deferred — game running, Known regression, Recovery available, Unknown changed state, Unprotected change, Missing save, Queue inconsistency, Manifest inconsistency, Error.
Bidirectional Sync: Off / Ask (default) / Automatic Safe Cases. Regression Recovery: Off / Ask (default) / Certain Only. Rollback Before Restore: On (default).

## Logging
Bounded, timestamped; no noisy per-poll logs. Events: GAME_START, GAME_EXIT, SAVE_STABILITY_SAMPLE, SAVE_STILL_CHANGING, SAVE_STABLE, SAVE_STABILITY_TIMEOUT, SAVE_CHANGED, SAVE_UNCHANGED, BACKUP_STARTED, BACKUP_PACKAGED, BACKUP_QUEUED, PC_DOWNLOAD_STARTED, PC_DOWNLOAD_VERIFIED, PC_ACK, REST_REQUEST_DETECTED, REST_BACKUP_STARTED, SHUTDOWN_REQUEST_DETECTED, SHUTDOWN_BACKUP_STARTED, SHUTDOWN_BACKUP_TIMEOUT, IDLE_SCAN_STARTED, IDLE_SCAN_COMPLETED, IDLE_SCAN_CHANGED_COUNT, STARTUP_RECONCILIATION_STARTED, QUEUE_RECOVERED, INTERRUPTED_OPERATION_FOUND, STALE_TMP_REMOVED, KNOWN_REGRESSION_DETECTED, SYNC_CONFLICT_DETECTED, STARTUP_RECONCILIATION_COMPLETED, RECONCILE_TITLE_SANITY_STARTED/COMPLETED, RECONCILE_TITLE_FULL_STARTED/COMPLETED, RECONCILE_CATALOGUE_QUICK_STARTED/COMPLETED, RECONCILE_CATALOGUE_FULL_STARTED, RECONCILE_CATALOGUE_PROGRESS, RECONCILE_CATALOGUE_FULL_COMPLETED, RECONCILE_CANCELLED, RECONCILE_ANOMALY_FOUND, RESTORE_STAGED, ROLLBACK_CREATED, RESTORE_STARTED, RESTORE_VERIFIED, RESTORE_FAILED.

## Session continuity
Maintain `CLAUDE_PROGRESS.md` (concise, factual): branch, commit/working-tree state, upstream base, firmware 13.60, current stage, completed/partial work, files changed, architectural decisions, APIs, state/config schema, build command, last build result, known issues, tests done/required, verified/unverified assumptions, next task, dangerous areas. Checklist statuses: NOT STARTED, IN PROGRESS, IMPLEMENTED, BUILDS, TESTED ON HOST, TESTED ON PS5, BLOCKED. Never mark TESTED because it compiles. Before stopping: finish smallest safe unit, build/test, inspect diff, update progress, record incomplete work precisely. New session: read progress, git status/log/diff, checklist; summarize implemented/incomplete/next; don't restart, don't redo, don't replace working code. Small logical commits.

## Workflow
First an investigation report (18 points — see INVESTIGATION.md). Then stages:
A persistent generalized backup queue · B save stability detector · C game-close automatic backup · D PC HTTP queue downloader · E persistent fingerprints/generations · F startup reconciliation · G per-title sanity check · H per-title full reconciliation · I quick catalogue reconciliation · J full catalogue reconciliation · K idle changed-save sweep · L bidirectional manifest exchange · M conservative PC→PS5 restore · N regression recovery · O system-state observation · P Safe Rest/Shutdown/Reboot if needed · Q optional cloud replication.
After each: build, fix errors, inspect diff, host tests where possible, update progress, small commit, no unrelated rewrites.

## Test plan (13.60) — to deliver at the end
Basic game close; fast save (no needless wait); slow save (waits until stable); stability timeout (no silent success, retry later); no change (no duplicate archive); PC offline then reconnect; kernel panic/restart (queued ZIPs survive, state rebuilt); panic regression (B offered); unknown branch (CONFLICT/UNKNOWN, no auto restore); idle sweep (only changed archived); PC→PS5 safe restore (rollback → restore → verify → manifest); restore while game running (deferred); per-game sanity; per-game full reconcile; quick catalogue; full catalogue; catalogue + protect; game starts during catalogue scan; Rest Mode (which path fires, safe?); shutdown/reboot; queue interruption (.tmp not exposed); PC interrupted download (.part retried, source kept); concurrent ops (no duplicate/corrupt archive); session continuation.

## Final deliverables
Built ELF path; PC client path; files changed; commit list; architecture summary; HTTP API; config schema; manifest/state schema; queue format; reconciliation state model; sync state model; PC setup; Windows direct-Ethernet notes; manual reconciliation guide; recovery workflow; 13.60 test checklist; known limitations; what could not be validated without the PS5. Never claim 13.60 verification without an actual test there.
