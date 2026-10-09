# Games, save objects and files

Garlic's LAN backup thinks in three levels:

| Level | What it is | Used for |
|---|---|---|
| **Game** (title) | everything one title keeps under `savedata_prospero/<TID>/` (or `savedata/<CUSA>/`) per user | the default backup and restore unit: **snapshots** |
| **Save object** | one logical save: the primary image `sdimg_<X>` plus, when present, the PS5's own backup copy `sdimg_sce_bu_<X>` (PS4: plus the `<X>.bin` key file) | individual backup / restore without touching sibling saves |
| **File** | one `sdimg_*` image | storage unit underneath: archives, generations, PC sync state |

Examples seen on the owner's console:
- TLOU2: `SAVEFILE00…03, 0A, 0P` (each with `sce_bu_`): 6 objects.
- Digimon: `sysdata.bin, 0000.bin … 0006.bin` (with `sce_bu_`): 8 objects.
- Atomfall: `Save00, 04, 05, 06, 08`: 5 objects.
- Some games: just `system` + `sce_bu_system`: 1 object.

## Object identity (no hard-coded slot names)
`sdimg_` is stripped; a following `sce_bu_` (non-empty rest) marks the backup copy; the rest is the object id, kept verbatim (`Save04`, `0000.bin`, `system`). Code: `ls_object_of()` / `ls_group_objects()` in `src/lansync_save.inc.c`.
- A primary without a backup copy is a normal object.
- A backup copy without a visible primary is an **orphan** object: it is backed up and shown (badge "backup copy only"), but never restored next to a primary that exists on the PS5 (see restore).

## Where the per-file model used to leak through (before this change)
The old code was not "one save per title": it enumerated every `sdimg_*`. The problem was that it treated **every file as an independent save**:
1. `ls_enum_saves` → `ls_protect_title` looped over files; `sdimg_sce_bu_X` got its own archive, generation chain, sync state, regression detection and UI row.
2. There was no record of which file versions belonged together, so a game could not be put back to one point in time.
3. Restore (`ls_restore_request`/`ls_restore_run`) took exactly one file from one archive; restoring `sdimg_X` left its `sce_bu_X` behind, and notifications said "sdimg_0000 restored".
4. Reconcile/Compare/History/Games rows were per file.
5. Each file's packaging was individually consistent, but nothing proved the whole set was coherent.

## Snapshots (schema v6)
`snapshots(snap_id, user_id, title_id, kind 'title'|'object', object_id, created_at, reason, objects, files, fingerprint)` and `snapshot_files(snap_id, object_id, role 'primary'|'sce_backup', save_name, is_ps4, gen_id, sha256, size)`.

A snapshot does not copy data again: it references the archived **generation** of every file (one archive per changed file, as before; unchanged files are never re-archived).

Backup of a game (`ls_protect_title_ex`: game close, follow-up, idle sweep, startup, **Back Up Entire Game**):
1. title-level stability wait (unchanged: N equal samples of the title signature, before the job runs);
2. signature **before** packaging;
3. archive every changed file (each re-checked after zipping, as before);
4. signature **after**; only if equal and every file is protected is the snapshot committed. Otherwise `SNAPSHOT_DEFERRED` and the job's normal retry runs;
5. identical state → the newest snapshot is reused (no duplicates).

Backup of one object (**Back Up This Save**, job `save_object`): the same rules on the object's files (fingerprint over primary + copy), kind `object`.

Legacy data: migration v6 turns the last protected state of each (user, game) into one `migrated` game snapshot, so existing backups are restorable as a set. Older per-file archives stay readable and restorable one file at a time (History).

### Manifest (`GET /api/backup/snapshots`, stored by the PC client as `<TITLE>/_snapshots/<id>.json`)
```json
{"format":"garlic-snapshot","version":1,"snapshot_id":31,"kind":"title","object_id":"",
 "game":{"title_id":"PPSA15493","name":"Atomfall"},"user_id":"12229390","created_at":1791337815,
 "reason":"game_close","object_count":5,"file_count":9,"restorable_on_ps5":true,"all_on_pc":false,
 "objects":[{"id":"Save04","members":[
   {"role":"primary","file":"sdimg_Save04","is_ps4":false,"gen_id":21,"sha256":"…","size":73334942,
    "archive_id":"20261007-035012_PPSA15493_sdimg_Save04_0005","archive_on_ps5":true,"on_pc":false},
   {"role":"sce_backup","file":"sdimg_sce_bu_Save04", "…":"…"}]}]}
```
Relative file names only; `archive_id` is the queue id (= the PC's archive key).

## Restore semantics
Kinds: **file** (legacy History row; unchanged behaviour), **object** (one object from a snapshot), **title** (whole game snapshot). `ls_restore_run`:
1. **Stage + validate every file** (archive hash, `param.sfo` title / directory / account on a scratch copy, re-sign account-0 archives on a copy). Any failure: nothing on the PS5 changed, staging deleted.
2. **Back up the whole game as it is** (`pre_restore`, a normal game snapshot = the point to go back to) + byte copies of the files about to be replaced. If anything cannot be protected, the restore stops before replacing anything.
3. **Swap** file by file (PS4 key first; title-running check before each).
4. **Verify** every placed file by hash.
5. Any failure in 3–4 **undoes every file placed so far**: previous bytes put back, and files that did not exist before are removed again (the only removal a restore ever does). Result `failed` with per-file results (`undone`, `failed`), or `partial` if an undo itself failed (message names the files and the pre-restore snapshot to restore).
6. Record: `savedata.db` for primaries only, protection bookkeeping through the normal protect path (re-signed content gets its own archive), a new game snapshot.

**Deletion policy (explicit):** a restore never deletes a save.
- Objects on the PS5 that are not in the snapshot are left as they are (`RESTORE_KEPT`).
- A file of an object that the snapshot lacks (e.g. the snapshot had no `sce_bu_` copy) is left as it is.
- A backup copy whose primary is not part of the restore is **skipped** if a primary exists on the PS5 (never mixes two points in time inside one object).

**Exact game restore (UI default since 2026-10-07, owner decision).** When a whole-game snapshot is restored and the PS5 has saves the snapshot does not contain, the UI lists them; the highlighted default is *Restore exactly (remove these saves)* (games choose autosaves / newest slots / shared slot files themselves, so leftover newer saves make a partial restore unreliable); *Restore but keep these saves* is the alternative. A final confirmation names each save. The API stays explicit: without `exact=1` nothing is removed.
- `GET /api/restore/snapshot_preview?id=` returns `restore_objects`, `remove[{object_id, files}]` and `remove_fp` (SHA-256 of the removal list).
- `POST /api/restore/snapshot?id=&exact=1&remove_fp=` is refused (`-8`) if the files on the PS5 differ from what was confirmed.
- Each file to remove must be archived by the pre-restore backup (generation with that exact hash, archive on the PS5 or on the PC), else nothing is replaced. Byte copies are kept; removals run only after every placement succeeded; a later failure puts them back. Removed primaries lose their `savedata.db` row (as upstream `delete_save`); saves rows become `missing`.
- Undo: restore the pre-restore game snapshot named in the result.
- Events: `RESTORE_REMOVAL_CONFIRMED`, `RESTORE_REMOVED`. Only whole-game restores can be exact.

**Delete one save** (Saves -> Delete This Save, `POST /api/backup/delete_object?id=&object=[&user=]`): runs through the same pipeline (kind `delete`): whole game backed up first (`pre_delete`), each file must be archived, save file + PS5 backup copy + savedata.db row removed, undone on failure. The Saves view lists "Deleted saves" (objects in snapshots but not on the PS5) with Versions / Restore to bring them back.

Notifications: "restored save Save04 for Atomfall", "restored 5 save object(s) for Atomfall", "backed up save Save04 of Atomfall".

## API
- `GET /api/backup/objects?title=` — objects with `files.primary` / `files.sce_backup` (state, current gen, size, PC pending), aggregate `state`, `orphan`, per-object `fingerprint`.
- `GET /api/backup/snapshots?title=` (newest first) or `?since=<snap_id>` (all titles, oldest first, max 100; PC feed).
- `POST /api/backup/protect_object?id=<TID>&object=<id>`; `POST /api/backup/protect_title?id=<TID>` (= Back Up Entire Game).
- `POST /api/restore/snapshot?id=<snap>[&object=<id>][&exact=1&remove_fp=]`; refused with `-7` and the file names if an archive is no longer on the PS5, `-8` if an exact confirmation is stale. `GET /api/restore/snapshot_preview?id=`.
- `GET /api/restore/status` adds `kind, object_id, snap_id, rollback_snap, kept_objects, items[{save_name, object_id, role, gen_id, result, note}]`.
- History, saves, sanity/full reconcile and compare rows carry `object_id` and `role`; `/api/backup/titles` reports `objects` next to `saves` (files).

## Tests
Host: `test_lansync_objects` (naming, single/multi/extension/primary-only/orphan grouping, per-object fingerprints), `test_lansync_snapshots` (coherent game snapshot, no duplicates, deferral when a file moves mid-backup, object backup, orphans), `test_lansync_restore_multi` (whole game, object without touching siblings, failure halfway undoes everything, missing archives, orphan skip, migrated legacy snapshot), `test_lansync_api` (endpoints), `test_lansync_migrate` (v6 backfill), `test_lansync_restore` (legacy single-file restore unchanged). PC: snapshot manifests stored once, old Garlic tolerated.

## Not done yet / limits
- Restoring a game snapshot whose archives are only on the PC (pruned from the PS5 after `keep_acked_hours`) is not implemented; the PC holds the manifest and archives, but `garlic_sync.py --restore` still uploads one file at a time.
- The PC compare (stage L) is still classified per file; the UI labels rows by object.
- Undo failure (`partial`) is handled but not exercised by a host test (needs a filesystem fault).
- Nothing here has run on the PS5 yet; in particular whether a restored `sce_bu_` copy is accepted as-is by the system is unverified.

## Regression recovery (stage N)
When a backup run finds a save whose content equals an older archived version than the newest one (e.g. a crash left the previous save on disk), Garlic records one offer per save object (`recovery` table, schema v7):
- **A** = what is on the PS5 now (already archived, nothing is lost); **B** = the newest snapshot before that run holding the newer versions.
- **Certain** only if every regressed file's version is an ancestor of B's version (generation parent chain), no file of the save got new content in the same run, and B's archives are still on the PS5.
- Setting `regression_recovery`: `off` = record + event only; `ask` (default) = notification, the UI offers *Restore newer version* / *Keep current version*; `certain` = restore B automatically when certain, otherwise ask.
- Deliberate restores and deletes never create offers. New content on a save with an open offer marks it `stale` (restoring B would also replace that progress, which is archived). A newer regression supersedes the open offer.
- API: `GET /api/recovery[?title=]`, `POST /api/recovery/resolve?id=&action=restore|keep`; `/api/backup/status` and `/api/backup/titles` report `recovery_open`. Events: `REGRESSION_DETECTED`, `REGRESSION_AUTO_RESTORE`, `REGRESSION_AUTO_RESTORE_REFUSED`, `REGRESSION_RESOLVED`.
- Test aid: `POST /api/restore/snapshot?id=<snap>&object=<id>&simulate_regression=1` places an older version through the restore pipeline but books it as an unexplained change, so it produces an offer like a crash would (event `REGRESSION_SIMULATION`). Not in the UI.
- Limit: only byte-identical regressions are recognised. On the owner's data the PS5's own `sce_bu` copies are never byte-identical to a save-file version, so a rollback by the system from its backup copy would look like new content (archived normally, no offer).
