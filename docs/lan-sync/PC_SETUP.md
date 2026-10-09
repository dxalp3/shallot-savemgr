# PC setup — Garlic LAN sync client (Windows)

The PC pulls finished save archives from Garlic over the LAN. No FTP server and no internet are needed. The PS5 keeps every archive until this client has stored it, verified its SHA-256 and acknowledged it.

## 1. Requirements
- Python 3.8+ (`python --version`). No extra packages.
- The PS5 running Garlic SaveMgr with LAN sync (`/api/backup/status` answers).

## 2. Direct Ethernet (PS5 ⇄ PC cable, no router)
Without a router there is no DHCP, so give both sides fixed addresses in the same subnet:
- **PC:** Settings → Network & Internet → Ethernet → IP assignment → Edit → Manual → IPv4 on: IP `192.168.50.1`, mask `255.255.255.0`, no gateway, no DNS.
- **PS5:** Settings → Network → Settings → Set Up Internet Connection → (wired) → Advanced Settings → IP Address Settings: Manual, IP `192.168.50.2`, mask `255.255.255.0`, gateway `192.168.50.1`, DNS `192.168.50.1` (the PS5 will report no internet; that is fine).
- Windows may classify the link as a *Public* network and block replies. The client only makes **outgoing** connections to port 8082, which Windows allows by default.
- Check from the PC: open `http://192.168.50.2:8082/api/backup/status` in a browser.

With a normal router, just use the PS5's LAN address instead.

## 3. Configure
```
cd <repo>\pc
copy config.example.json config.json
```
Edit `config.json`: set `ps5_host` (e.g. `192.168.50.2`) and `backup_dir` (absolute path recommended, e.g. `D:\\PS5Saves`). `config.json` is gitignored.

## 4. Run
- One pass (good for testing): `python garlic_sync.py --once -v`
- Continuous: `python garlic_sync.py` (polls every `poll_interval_s`, logs once per outage when the PS5 is off).

## 5. Start automatically (Task Scheduler)
```
schtasks /Create /TN "Garlic LAN Sync" /SC ONLOGON /RL LIMITED /TR "\"C:\Path\To\pythonw.exe\" \"C:\Path\To\garlic-savemgr\pc\garlic_sync.py\""
```
`pythonw.exe` runs without a console window; check `pc\garlic_sync.log` for activity. Remove with `schtasks /Delete /TN "Garlic LAN Sync"`.

## 6. What ends up on disk
```
<backup_dir>\manifest.json
<backup_dir>\<TITLE>\<SAVE>\<yyyymmdd-hhmmss>_<TITLE>_<SAVE>_<seq>.zip   (= Garlic queue id)
```
- Files are never overwritten or deleted by the client.
- Each ZIP is the same encrypted, restorable archive Garlic creates for its other backup targets: `<TITLE>/<save image>` (+ `<TITLE>/<name>.bin` for PS4 saves).
- `manifest.json` lists every stored archive with its SHA-256 and whether the PS5 acknowledged it.

## Failure behaviour
| Situation | Result |
|---|---|
| PS5 off / cable unplugged | client logs once, retries next poll; PS5 keeps the archive queued |
| Download interrupted / short | `.part` deleted, no ack, retried next poll |
| SHA-256 mismatch | `.part` deleted, no ack, retried next poll |
| Ack lost after storing | next poll re-verifies the stored file and re-sends the ack (no second download) |
| Target file exists with different bytes | refuses to overwrite, no ack, logged |

## Restoring a version to the PS5
Close the game first. Garlic always backs up the version currently on the PS5 before it restores anything, validates the archive on a scratch copy (title, save, account) and verifies the result by hash.

- **Version still on the PS5:** web UI → Backup & Sync → game → History → "Restore this version".
- **Version only on the PC (or a ZIP from elsewhere):**
  ```
  python garlic_sync.py --list PPSA01234
  python garlic_sync.py --restore <id from the list>
  python garlic_sync.py --restore D:\somewhere\archive.zip --save sdimg_SAVE0 --user 1a2b3c4d
  ```
  `--save` is needed for archives with several saves, `--user` for ZIPs that do not record the PS5 user (the hex folder name under `/user/home`).
- Only archives of the same PS5 account can be restored this way; for another account use Garlic's Import tab (it re-signs the save).
- Only uncompressed ZIPs (as Garlic writes them) are accepted.

## Thinning out old versions (retention)
The client keeps every version for `keep_all_days` (2), then the newest version of each day up to `daily_days` (30), then the newest of each week (`weekly: false` drops those instead). Units are Garlic's game / save snapshots: an archive is kept while any kept snapshot needs it (an unchanged slot shared by many snapshots stays), and the newest version of every save file, archives you placed in the folder yourself (external) and archives not yet acknowledged are never thinned. Archives made before snapshots existed are thinned per save file with the same rule.

Thinned files go to `<backup_dir>/_trash/` and are deleted after `trash_days` (14). The running client thins out every `interval_h` (6) hours; set `"retention": {"enabled": false}` to switch it off.
```
python garlic_sync.py --prune --dry-run     # list what would be thinned out
python garlic_sync.py --prune               # do it now
```

## Block store (dedup)
Consecutive versions of a save usually differ in a few 4 KiB blocks (measured on Atomfall: 3 of 17,904), so the client keeps each archive as a list of block hashes and stores every distinct block once in `<backup_dir>/_blocks/` (append-only `pack-*.dat` files + `blocks.db`). Blocks start where the save image begins inside the ZIP, so they line up with the image's own blocks.

- A downloaded ZIP is moved in right after it is stored and acknowledged; the ZIP is deleted only after the archive was rebuilt from the blocks and matched its SHA-256. Existing ZIPs move in gradually (20 per pass) or all at once with `python garlic_sync.py --dedup`.
- Restores rebuild the exact original bytes and check the hash again before uploading. `python garlic_sync.py --export <id> out.zip` writes any version back out as a normal ZIP.
- ZIPs you placed in the folder yourself (external) and archives not yet acknowledged stay ZIPs.
- Thinning (above) still decides which versions are kept; when a thinned version is purged from the trash, blocks no other version uses are dropped and mostly-empty pack files are rewritten.
- Measured on the owner's store (2026-10-07): 202 archives, 3.71 GB -> 789 MB, every archive verified.
- Switch off with `"dedup": {"enabled": false}`; existing block copies stay readable.
