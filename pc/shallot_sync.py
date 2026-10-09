#!/usr/bin/env python3
"""
Shallot SaveMgr LAN sync — PC client (stdlib only, Python 3.8+).
Shallot (dxalp3) is a fork of Garlic SaveMgr (earthonion): https://git.etawen.dev/earthonion/garlic-savemgr

Pulls finished save archives from Shallot's persistent backup queue over the
LAN and stores them durably on this PC. The PS5 forgets nothing until this
client has written, verified and fsync'd the archive and then acknowledged
it with the archive's SHA-256.

    python shallot_sync.py                 # run forever (waits for new archives)
    pythonw shallot_sync.py --tray         # same, with a tray icon (needs: pip install pystray pillow)
    python shallot_sync.py --autostart on  # start with Windows (tray) and start it now; "off" removes it
    python shallot_sync.py --once          # drain the queue once and exit
    python shallot_sync.py --config path/to/config.json
    python shallot_sync.py --list PPSA01234          # versions this PC holds for a title
    python shallot_sync.py --restore <id|zip path> [--save sdimg_X] [--user 1a2b3c4d]
                                                  # push one version back to the PS5
    python shallot_sync.py --prune [--dry-run]     # thin out old versions now (see RETENTION)
    python shallot_sync.py --dedup                 # move every stored archive into the block store now
    python shallot_sync.py --export <id> <file.zip>  # write one archive back out as a normal ZIP
    python shallot_sync.py --vault-pull            # rebuild this PC's store from the VPS vault
    python shallot_sync.py --vault-gc              # delete vault blocks no archive needs any more

Off-site copy (optional, "vault" in config.json; needs: pip install cryptography):
every archive this PC holds is also kept in a Shallot vault on a VPS
(vault/ in the repository: Docker image + HTTPS proxy). Blocks are
encrypted here with AES-256-GCM under a key that never leaves this PC
(vault.key next to config.json; keep a copy elsewhere, without it the
vault is unreadable). Block names are keyed hashes, so the vault
deduplicates without learning anything. Uploads run in short slices of the
normal loop; "Get from PC" on the PS5 falls back to the vault when this PC
no longer has an archive.

Layout: <backup_dir>/<TITLE>/<SAVE>/<queue id>.zip   (id = <stamp>_<TITLE>_<save>_<seq>)
Existing files are never overwritten. <backup_dir>/manifest.json records
every stored archive (written atomically).

Block store (dedup): consecutive versions of a save differ in a handful of
4 KiB blocks, so archives are kept as a list of block hashes plus the
blocks themselves, stored once in <backup_dir>/_blocks (append-only pack
files + blocks.db). An archive's ZIP is deleted only after it was rebuilt
from the blocks and matched its SHA-256; restores and --export rebuild the
exact original bytes and check them again.

After storing, the client reports to Shallot, per title, every archive it
holds (also ZIPs placed into the backup folder by hand, marked "external")
together with the SHA-256 of each save image inside, so Shallot can tell
SYNCED / PS5_NEWER / PC_NEWER / CONFLICT / regressions per save.
"""
import argparse
import datetime
import socket
import threading
import hashlib
import json
import logging
import logging.handlers
import os
import re
import shutil
import sqlite3
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

DEFAULT_CONFIG = {
    "ps5_host": "192.168.1.50",
    "port": 8082,
    "backup_dir": "ShallotBackups",
    "poll_interval_s": 30,
    "verify_downloads": True,
    "http_timeout_s": 60,
    "log_file": "shallot_sync.log",
    # Thinning of old versions on this PC (see thin_out()).
    "retention": {
        "enabled": True,
        "keep_all_days": 2,      # every version younger than this is kept
        "daily_days": 30,        # then the newest version per day up to this age
        "weekly": True,          # then the newest version per week, forever (False: drop older)
        "trash_days": 14,        # thinned files wait in <backup_dir>/_trash this long before deletion
        "interval_h": 6,         # how often the running client thins out
    },
    # Off-site copy on a VPS (see VAULT); empty url = off.
    "vault": {
        "url": "",
        "token": "",
        "key_file": "vault.key",
        "interval_min": 10,       # look for new archives / snapshots to upload at least this often
        "slice_s": 20,            # upload at most this long per loop pass, then serve the PS5 again
    },
    # Block-level dedup of stored archives (see BlockStore).
    "dedup": {
        "enabled": True,
        "block_size": 4096,
        "pack_mb": 256,          # size of one append-only pack file
    },
}

CLIENT_VERSION = "2.4"
RESTART = 75                     # run_forever/run_tray: start this script again (after "update")
CHUNK = 1024 * 1024
WAIT_S = 25                      # how long Shallot holds a /api/backup/wait request
log = logging.getLogger("shallot_sync")


class TransferError(Exception):
    """Download or verification failed; the PS5 queue entry is left untouched."""


ENV_OVERRIDES = {          # for containers: settings without editing config.json
    "SHALLOT_PS5_HOST": ("ps5_host", str), "SHALLOT_PS5_PORT": ("port", int), "SHALLOT_BACKUP_DIR": ("backup_dir", str),
    "SHALLOT_CLIENT_KEY": ("client_key", str),
    "SHALLOT_LOG_FILE": ("log_file", str),
}


def load_config(path):
    cfg = dict(DEFAULT_CONFIG)
    if path and os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            cfg.update(json.load(f))
    for env, (key, conv) in ENV_OVERRIDES.items():
        if os.environ.get(env):
            cfg[key] = conv(os.environ[env])
    vault_env = {k: os.environ[e] for e, k in (("SHALLOT_VAULT_URL", "url"), ("SHALLOT_VAULT_TOKEN", "token"),
                                               ("SHALLOT_VAULT_KEY_FILE", "key_file")) if os.environ.get(e)}
    if vault_env:
        cfg["vault"] = dict(cfg.get("vault") or {}, **vault_env)
    base = os.path.dirname(os.path.abspath(path)) if path else os.getcwd()
    for key in ("backup_dir", "log_file"):
        if cfg.get(key) and not os.path.isabs(cfg[key]):
            cfg[key] = os.path.join(base, cfg[key])
    v = dict(DEFAULT_CONFIG["vault"], **(cfg.get("vault") or {}))
    if v.get("key_file") and not os.path.isabs(v["key_file"]):
        v["key_file"] = os.path.join(base, v["key_file"])
    cfg["vault"] = v
    return cfg


def safe_component(value, fallback):
    value = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", value or "").strip(" .")
    return value[:120] or fallback


def fsync_dir(path):
    """Best effort: directories cannot be fsync'd on Windows."""
    if os.name == "nt":
        return
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def atomic_write_json(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, sort_keys=True)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    fsync_dir(os.path.dirname(path))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(CHUNK), b""):
            h.update(block)
    return h.hexdigest()


class ShallotClient:
    def __init__(self, cfg):
        self.cfg = cfg
        self.base = "http://%s:%d" % (cfg["ps5_host"], int(cfg["port"]))
        self.timeout = float(cfg.get("http_timeout_s", 60))
        # With the web UI login on, Shallot wants the PC client key on every request.
        self.opener = urllib.request.build_opener()
        if cfg.get("client_key"):
            self.opener.addheaders = [("X-Shallot-Key", str(cfg["client_key"]))]

    def _open(self, req, timeout):
        try:
            return self.opener.open(req, timeout=timeout)
        except urllib.error.HTTPError as e:
            if e.code == 401:
                raise TransferError("Shallot's web UI login is on: add \"client_key\" to config.json (or "
                                    "SHALLOT_CLIENT_KEY for Docker); it is shown under Settings > Web UI login")
            raise

    def _url(self, path, **params):
        if getattr(self, "cid", ""):
            params.setdefault("cid", self.cid)
        q = urllib.parse.urlencode(params)
        return self.base + path + ("?" + q if q else "")

    def get_json(self, path, **params):
        with self._open(self._url(path, **params), timeout=self.timeout) as r:
            data = json.loads(r.read().decode("utf-8"))
        if isinstance(data, dict) and data.get("error"):
            raise TransferError("%s: %s" % (path, data["error"]))
        return data

    def post_json(self, path, body=b"", **params):
        req = urllib.request.Request(self._url(path, **params), data=body, method="POST")
        with self._open(req, timeout=self.timeout) as r:
            data = json.loads(r.read().decode("utf-8"))
        if isinstance(data, dict) and data.get("error"):
            raise TransferError("%s: %s" % (path, data["error"]))
        return data

    def pending(self):
        return self.get_json("/api/backup/queue", pending="1").get("queue", [])

    def open_download(self, item_id):
        return self._open(self._url("/api/backup/queue_download", id=item_id),
                                      timeout=self.timeout)

    def ack(self, item_id, sha256):
        return self.post_json("/api/backup/queue_ack", id=item_id, dest="pc", sha256=sha256)

    def upload_restore(self, path, title_id, save_name, user_id, archive_sha, container_sha):
        """Streams an archive to /api/restore/upload (the PS5 verifies and restores it)."""
        url = self._url("/api/restore/upload", title=title_id, save=save_name, user=user_id,
                        archive_sha256=archive_sha, container_sha256=container_sha or "")
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            req = urllib.request.Request(url, data=f, method="POST",
                                         headers={"Content-Length": str(size),
                                                  "Content-Type": "application/zip"})
            with self._open(req, timeout=max(self.timeout, 300)) as r:
                data = json.loads(r.read().decode("utf-8"))
        if isinstance(data, dict) and data.get("error"):
            raise TransferError(data["error"])
        return data

    def restore_status(self):
        return self.get_json("/api/restore/status").get("restore", {})

    def snapshots(self, since):
        """Game / save-object snapshot manifests newer than `since` (oldest first, max 100)."""
        return self.get_json("/api/backup/snapshots", since=str(int(since))).get("snapshots", [])

    def post_manifest(self, title_id, entries):
        body = json.dumps({"archives": entries}).encode("utf-8")
        return self.post_json("/api/backup/pc_manifest", body=body, title=title_id)

    def wait(self, seq, timeout=WAIT_S, info=None):
        """Returns when Shallot queues an archive, a sync is requested or the web
        UI leaves a command (or after `timeout`): {"seq", "reason", "pending"
        [, "cmd", "cmd_id"]}. None if this Shallot predates it. `info`: what this
        client tells the web UI about itself (kind, paused, store size, ...)."""
        v = getattr(self, "vault_state", None) or {}
        url = self._url("/api/backup/wait", seq=str(int(seq)), timeout=str(int(timeout)),
                        client=(os.environ.get("SHALLOT_CLIENT_NAME") or socket.gethostname())[:60], version=CLIENT_VERSION,
                        vault=v.get("state", "off"), vault_pending=str(int(v.get("pending", 0))),
                        vault_at=str(int(v.get("at", 0))), vault_src=v.get("src", ""), **(info or {}))
        try:
            with self._open(url, timeout=timeout + 15) as r:
                data = json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            raise
        if not isinstance(data, dict) or "seq" not in data:
            return None
        return data

    def kick(self):
        return self.post_json("/api/backup/kick")

    def command_done(self, cmd_id, ok, result):
        """Tells the web UI how a command it sent went."""
        try:
            self.post_json("/api/pc/command_done", id=str(int(cmd_id)), ok="1" if ok else "0", result=str(result)[:180])
        except (urllib.error.URLError, OSError, TransferError, ValueError) as e:
            log.warning("could not report command %s: %s", cmd_id, e)

    def vault_config(self):
        """The PS5's vault settings when it wants this PC to upload ("PC and
        VPS", PC route): {"route", "url", "token", "key"}; None if unsupported."""
        try:
            return self.get_json("/api/pc/vault_config")
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            raise
        except TransferError:
            return None

    def vault_acks(self, ids):
        """Tells Shallot these archives reached the vault. False if Shallot predates it."""
        try:
            self.post_json("/api/pc/vault_acks", body=json.dumps({"ids": ids}).encode("utf-8"))
            return True
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return False
            raise

    def client_script(self):
        """The sync client bundled with the running Shallot build."""
        with self._open(self._url("/api/pc/shallot_sync.py"), timeout=self.timeout) as r:
            return r.read()

    def requests(self):
        """Archives Shallot asks back (a restore of a version the PS5 no longer
        holds). None if this Shallot predates the feature."""
        try:
            return self.get_json("/api/pc/requests").get("requests", [])
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            raise
        except TransferError:
            return None

    def readopt(self, item_id, path):
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            req = urllib.request.Request(self._url("/api/pc/readopt", id=item_id), data=f, method="POST",
                                         headers={"Content-Length": str(size), "Content-Type": "application/zip"})
            with self._open(req, timeout=max(self.timeout, 300)) as r:
                data = json.loads(r.read().decode("utf-8"))
        if isinstance(data, dict) and data.get("error"):
            raise TransferError(data["error"])
        return data


def archive_contents(path):
    """Save images inside an archive: [{"title_id", "save_name", "sha256"}].

    Shallot archives hold "<TITLE>/<sdimg_name>" (+ "<TITLE>/<name>.bin" key
    files for PS4 saves, which are not save images). Non-ZIP files -> [].
    """
    out = []
    try:
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                parts = info.filename.split("/")
                if len(parts) != 2 or not parts[1].startswith("sdimg_"):
                    continue
                h = hashlib.sha256()
                with z.open(info) as f:
                    for block in iter(lambda: f.read(CHUNK), b""):
                        h.update(block)
                out.append({"title_id": parts[0], "save_name": parts[1], "sha256": h.hexdigest()})
    except (zipfile.BadZipFile, OSError, KeyError):
        return []
    return out


def _image_offset(path):
    """Offset of the first save image's bytes inside a Shallot ZIP (0 if unknown).
    Blocks start there so they line up with the image's own 4 KiB blocks."""
    try:
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                if info.filename.split("/")[-1].startswith("sdimg_") and info.compress_type == zipfile.ZIP_STORED:
                    with open(path, "rb") as f:
                        f.seek(info.header_offset)
                        h = f.read(30)
                    if len(h) == 30 and h[:4] == b"PK\x03\x04":
                        return info.header_offset + 30 + int.from_bytes(h[26:28], "little") + int.from_bytes(h[28:30], "little")
    except (zipfile.BadZipFile, OSError):
        pass
    return 0


class BlockStore:
    """Content-addressed store of fixed-size blocks in append-only pack files.

    blocks.db: blocks(hash -> pack, offset, length), recipes(key -> size,
    sha256, block list). Pack bytes are fsync'd before the index rows that
    point at them are committed; a crash in between leaves unreferenced bytes
    (reclaimed by compaction), never a dangling reference.
    """

    def __init__(self, root, block_size=4096, pack_bytes=256 << 20):
        self.dir = os.path.join(root, "_blocks")
        os.makedirs(self.dir, exist_ok=True)
        self.block_size = int(block_size)
        self.pack_bytes = int(pack_bytes)
        # The store is created on the main thread but used by the sync thread
        # (--tray); only one thread uses it at a time.
        self.db = sqlite3.connect(os.path.join(self.dir, "blocks.db"), check_same_thread=False)
        self.db.execute("PRAGMA journal_mode=DELETE")
        self.db.execute("PRAGMA synchronous=FULL")
        self.db.executescript(
            "CREATE TABLE IF NOT EXISTS blocks(hash BLOB PRIMARY KEY, pack INTEGER, off INTEGER, len INTEGER);"
            "CREATE TABLE IF NOT EXISTS recipes(key TEXT PRIMARY KEY, size INTEGER, sha256 TEXT, block_size INTEGER,"
            " hashes BLOB);"
            "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v);")
        self._files = {}

    def close(self):
        for f in self._files.values():
            f.close()
        self._files = {}
        self.db.close()

    def _close(self, n):
        f = self._files.pop(n, None)
        if f:
            f.close()

    def _pack_path(self, n):
        return os.path.join(self.dir, "pack-%06d.dat" % n)

    def _meta(self, k, default=None):
        row = self.db.execute("SELECT v FROM meta WHERE k = ?", (k,)).fetchone()
        return row[0] if row else default

    def _current_pack(self):
        n = int(self._meta("pack", 1))
        if os.path.exists(self._pack_path(n)) and os.path.getsize(self._pack_path(n)) >= self.pack_bytes:
            n += 1
            with self.db:
                self.db.execute("INSERT OR REPLACE INTO meta(k, v) VALUES('pack', ?)", (n,))
        return n

    def has(self, key):
        return self.db.execute("SELECT 1 FROM recipes WHERE key = ?", (key,)).fetchone() is not None

    def recipe(self, key):
        row = self.db.execute("SELECT size, sha256, hashes FROM recipes WHERE key = ?", (key,)).fetchone()
        if not row:
            return None
        hashes = [row[2][i:i + 32] for i in range(0, len(row[2]), 32)]
        return row[0], row[1], hashes

    def put_file(self, key, path, expected_sha):
        """Stores the file's bytes as blocks under `key`, then proves the copy by
        rebuilding it. Returns new bytes written to packs."""
        if self.has(key):
            if self.sha_of(key) != expected_sha:
                raise TransferError("block copy of %s does not match its SHA-256" % key)
            return 0
        pack = self._current_pack()
        hashes, new = [], {}
        h = hashlib.sha256()
        size = written = 0
        first = _image_offset(path)
        with open(path, "rb") as f, open(self._pack_path(pack), "ab") as out:
            off = out.tell()
            while True:
                b = f.read(first if first else self.block_size)
                first = 0
                if not b:
                    break
                h.update(b)
                size += len(b)
                d = hashlib.sha256(b).digest()
                hashes.append(d)
                if d in new or self.db.execute("SELECT 1 FROM blocks WHERE hash = ?", (d,)).fetchone():
                    continue
                out.write(b)
                new[d] = (pack, off, len(b))
                off += len(b)
                written += len(b)
            out.flush()
            os.fsync(out.fileno())
        self._close(pack)          # reopen for reading: the pack grew
        if h.hexdigest() != expected_sha:
            raise TransferError("%s changed while it was read into the block store" % key)
        with self.db:
            self.db.executemany("INSERT OR IGNORE INTO blocks(hash, pack, off, len) VALUES(?,?,?,?)",
                                [(d,) + v for d, v in new.items()])
            self.db.execute("INSERT INTO recipes(key, size, sha256, block_size, hashes) VALUES(?,?,?,?,?)",
                            (key, size, expected_sha, self.block_size, b"".join(hashes)))
        if self.sha_of(key) != expected_sha:
            with self.db:
                self.db.execute("DELETE FROM recipes WHERE key = ?", (key,))
            raise TransferError("block copy of %s failed verification" % key)
        return written

    def _read_block(self, d):
        row = self.db.execute("SELECT pack, off, len FROM blocks WHERE hash = ?", (d,)).fetchone()
        if not row:
            raise TransferError("block %s missing from the block store" % d.hex()[:16])
        pack, off, ln = row
        f = self._files.get(pack)
        if f is None:
            f = self._files[pack] = open(self._pack_path(pack), "rb")
        f.seek(off)
        b = f.read(ln)
        if len(b) != ln or hashlib.sha256(b).digest() != d:
            raise TransferError("block %s is damaged" % d.hex()[:16])
        return b

    def iter_bytes(self, key):
        r = self.recipe(key)
        if not r:
            raise TransferError("%s is not in the block store" % key)
        for d in r[2]:
            yield self._read_block(d)

    def sha_of(self, key):
        h = hashlib.sha256()
        for b in self.iter_bytes(key):
            h.update(b)
        return h.hexdigest()

    def write_to(self, key, dst, expected_sha):
        """Rebuilds the original bytes into dst (.part -> fsync -> rename) and checks the hash."""
        part = dst + ".part"
        h = hashlib.sha256()
        try:
            with open(part, "wb") as out:
                for b in self.iter_bytes(key):
                    out.write(b)
                    h.update(b)
                out.flush()
                os.fsync(out.fileno())
            if h.hexdigest() != expected_sha:
                raise TransferError("rebuilt %s does not match its SHA-256" % key)
            os.replace(part, dst)
        except BaseException:
            if os.path.exists(part):
                os.remove(part)
            raise

    def delete(self, key):
        with self.db:
            self.db.execute("DELETE FROM recipes WHERE key = ?", (key,))

    def gc(self, min_dead_ratio=0.5):
        """Drops blocks no recipe uses and rewrites packs that are mostly dead.
        Returns bytes reclaimed on disk."""
        live = set()
        for (hs,) in self.db.execute("SELECT hashes FROM recipes"):
            live.update(hs[i:i + 32] for i in range(0, len(hs), 32))
        dead = [row[0] for row in self.db.execute("SELECT hash FROM blocks") if row[0] not in live]
        with self.db:
            self.db.executemany("DELETE FROM blocks WHERE hash = ?", [(d,) for d in dead])
        reclaimed = 0
        current = int(self._meta("pack", 1))
        for name in sorted(os.listdir(self.dir)):
            if not (name.startswith("pack-") and name.endswith(".dat")):
                continue
            n = int(name[5:11])
            path = self._pack_path(n)
            size = os.path.getsize(path)
            live_bytes = self.db.execute("SELECT COALESCE(SUM(len), 0) FROM blocks WHERE pack = ?", (n,)).fetchone()[0]
            if n == current and live_bytes:
                continue
            if live_bytes == 0:
                self._close(n)
                os.remove(path)
                reclaimed += size
                continue
            if size == 0 or (size - live_bytes) / size < min_dead_ratio:
                continue
            # Copy the live blocks into the current pack, then retire this one.
            rows = self.db.execute("SELECT hash, off, len FROM blocks WHERE pack = ?", (n,)).fetchall()
            target = self._current_pack()
            if target == n:
                with self.db:
                    self.db.execute("INSERT OR REPLACE INTO meta(k, v) VALUES('pack', ?)", (n + 1,))
                target = n + 1
            moved = []
            with open(path, "rb") as src, open(self._pack_path(target), "ab") as out:
                off = out.tell()
                for d, o, ln in rows:
                    src.seek(o)
                    b = src.read(ln)
                    if hashlib.sha256(b).digest() != d:
                        raise TransferError("block %s is damaged; compaction stopped" % d.hex()[:16])
                    out.write(b)
                    moved.append((target, off, ln, d))
                    off += ln
                out.flush()
                os.fsync(out.fileno())
            with self.db:
                self.db.executemany("UPDATE blocks SET pack = ?, off = ?, len = ? WHERE hash = ?", moved)
            self._close(n)
            self._close(target)
            os.remove(path)
            reclaimed += size - live_bytes
        return reclaimed

    def stats(self):
        n_blocks, stored = self.db.execute("SELECT COUNT(*), COALESCE(SUM(len), 0) FROM blocks").fetchone()
        n_rec, logical = self.db.execute("SELECT COUNT(*), COALESCE(SUM(size), 0) FROM recipes").fetchone()
        packs = sum(os.path.getsize(os.path.join(self.dir, f)) for f in os.listdir(self.dir) if f.startswith("pack-"))
        return {"archives": n_rec, "logical_bytes": logical, "blocks": n_blocks, "block_bytes": stored, "pack_bytes": packs}


class Store:
    """Durable archive store with an append-only manifest."""

    def __init__(self, root, dedup=None):
        self.root = root
        os.makedirs(root, exist_ok=True)
        cfg = dict(DEFAULT_CONFIG["dedup"], **(dedup or {})) if dedup is not None else None
        self.blocks = BlockStore(root, cfg["block_size"], cfg["pack_mb"] << 20) \
            if cfg and cfg.get("enabled") else None
        if self.blocks is None and os.path.exists(os.path.join(root, "_blocks", "blocks.db")):
            self.blocks = BlockStore(root)           # existing block copies must stay readable
        self.manifest_path = os.path.join(root, "manifest.json")
        self.manifest = {"version": 1, "archives": {}}
        if os.path.exists(self.manifest_path):
            with open(self.manifest_path, "r", encoding="utf-8") as f:
                self.manifest = json.load(f)
        self.manifest.setdefault("archives", {})

    def client_id(self):
        """This client's id for Shallot (one PC client collects; the others stand by).
        Kept with the backups, so a container keeps it across restarts."""
        if not self.manifest.get("client_id"):
            import uuid
            self.manifest["client_id"] = uuid.uuid4().hex[:24]
            atomic_write_json(self.manifest_path, self.manifest)
        return self.manifest["client_id"]

    def target_path(self, item):
        title = safe_component(item.get("title_id"), "UNKNOWN")
        save = safe_component(item.get("save_name"), "all")
        # The queue id is already "<stamp>_<TITLE>_<save>_<seq>" and unique.
        name = safe_component(item["id"], "id") + ".zip"
        return os.path.join(self.root, title, save, name)

    def known(self, item_id):
        return self.manifest["archives"].get(item_id)

    def in_blocks(self, e):
        return e.get("stored") == "blocks"

    def present(self, key, e):
        """The archive's bytes are on this PC (as a ZIP or in the block store)."""
        if e.get("trashed_at"):
            return False
        if self.in_blocks(e):
            return self.blocks is not None and self.blocks.has(key)
        return os.path.exists(os.path.join(self.root, e["path"]))

    def verify(self, key, e):
        """True if the stored bytes still hash to the recorded SHA-256."""
        if self.in_blocks(e):
            try:
                return self.blocks is not None and self.blocks.sha_of(key) == e["sha256"]
            except TransferError:
                return False
        path = os.path.join(self.root, e["path"])
        return os.path.exists(path) and sha256_file(path) == e["sha256"]

    def materialize(self, key, e):
        """Path of a ZIP with the archive's exact bytes; second value: temp file to delete."""
        if not self.in_blocks(e):
            path = e["path"] if os.path.isabs(e["path"]) else os.path.join(self.root, e["path"])
            return path, None
        fd, tmp = tempfile.mkstemp(prefix="shallot-", suffix=".zip")
        os.close(fd)
        try:
            self.blocks.write_to(key, tmp, e["sha256"])
        except BaseException:
            os.remove(tmp)
            raise
        return tmp, tmp

    def dedup(self, key):
        """Moves one stored ZIP into the block store (verified), then deletes the ZIP.
        Returns new bytes written to packs, or None if not applicable."""
        e = self.manifest["archives"].get(key)
        if (self.blocks is None or not e or self.in_blocks(e) or e.get("trashed_at")
                or e.get("origin") == "external" or not e.get("acked")):
            return None
        path = os.path.join(self.root, e["path"])
        if not os.path.exists(path):
            return None
        if "contents" not in e:
            e["contents"] = archive_contents(path)
        written = self.blocks.put_file(key, path, e["sha256"])
        e["stored"] = "blocks"
        atomic_write_json(self.manifest_path, self.manifest)    # record first, then delete the ZIP
        os.remove(path)
        try:
            os.rmdir(os.path.dirname(path))
        except OSError:
            pass
        return written

    def dedup_pending(self, limit=None):
        """Moves stored ZIPs into the block store; also finishes interrupted moves."""
        if self.blocks is None:
            return 0
        done = 0
        for key, e in list(self.manifest["archives"].items()):
            if self.in_blocks(e):
                leftover = os.path.join(self.root, e["path"])
                if os.path.exists(leftover) and self.blocks.has(key):
                    os.remove(leftover)        # crash between manifest write and delete
                continue
            if limit is not None and done >= limit:
                break
            try:
                if self.dedup(key) is not None:
                    done += 1
            except (TransferError, OSError) as ex:
                log.warning("block store: %s left as ZIP: %s", key, ex)
        if done:
            st = self.blocks.stats()
            log.info("block store: %d archive(s) moved in; %d archives = %.1f MB of saves stored in %.1f MB",
                     done, st["archives"], st["logical_bytes"] / 1e6, st["pack_bytes"] / 1e6)
        return done

    def record(self, item, path, sha256, size):
        self.manifest["archives"][item["id"]] = {
            "path": os.path.relpath(path, self.root),
            "sha256": sha256,
            "size": size,
            "title_id": item.get("title_id", ""),
            "save_name": item.get("save_name", ""),
            "user_id": item.get("user_id", ""),
            "created_at": item.get("created_at", 0),
            "reason": item.get("reason", ""),
            "stored_at": int(time.time()),
            "acked": False,
        }
        atomic_write_json(self.manifest_path, self.manifest)

    def ensure_contents(self):
        """Fill in the save-image hashes of archives stored by older versions."""
        changed = False
        for entry in self.manifest["archives"].values():
            if "contents" in entry:
                continue
            path = os.path.join(self.root, entry["path"])
            entry["contents"] = archive_contents(path) if os.path.exists(path) else []
            changed = True
        if changed:
            atomic_write_json(self.manifest_path, self.manifest)

    def scan_external(self):
        """Register ZIPs placed in the backup folder by hand (origin "external")."""
        known = {os.path.normcase(e["path"]) for e in self.manifest["archives"].values()}
        added = 0
        for dirpath, dirs, files in os.walk(self.root):
            dirs[:] = [d for d in dirs if not d.startswith("_")]     # _trash, _snapshots
            for name in files:
                if not name.lower().endswith(".zip"):
                    continue
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, self.root)
                if os.path.normcase(rel) in known:
                    continue
                contents = archive_contents(full)
                if not contents:
                    continue
                sha = sha256_file(full)
                self.manifest["archives"]["ext:" + sha] = {
                    "path": rel, "sha256": sha, "size": os.path.getsize(full),
                    "title_id": contents[0]["title_id"], "save_name": contents[0]["save_name"],
                    "user_id": "", "created_at": int(os.path.getmtime(full)), "reason": "external",
                    "stored_at": int(time.time()), "acked": False, "origin": "external",
                    "contents": contents,
                }
                added += 1
        if added:
            atomic_write_json(self.manifest_path, self.manifest)
            log.info("registered %d external archive(s)", added)
        return added

    def reports(self):
        """Per title: the entries Shallot's /api/backup/pc_manifest expects."""
        by_title = {}
        for key, e in self.manifest["archives"].items():
            for c in e.get("contents") or []:
                by_title.setdefault(c["title_id"], [])   # report titles even if now empty
            if not self.present(key, e):
                continue      # removed / thinned out: no longer held
            external = e.get("origin") == "external"
            for c in e.get("contents") or []:
                by_title.setdefault(c["title_id"], []).append({
                    "queue_id": "" if external else key,
                    "archive_sha256": e["sha256"],
                    "container_sha256": c["sha256"],
                    "title_id": c["title_id"],
                    "save_name": c["save_name"],
                    "user_id": "" if external else e.get("user_id", ""),
                    "created_at": int(e.get("created_at") or 0),
                    "origin": "external" if external else "ps5",
                })
        return by_title

    def mark_acked(self, item_id):
        entry = self.manifest["archives"].get(item_id)
        if entry and not entry.get("acked"):
            entry["acked"] = True
            entry["acked_at"] = int(time.time())
            atomic_write_json(self.manifest_path, self.manifest)


def fetch_one(client, store, item, verify=True):
    """Store one queue item durably and acknowledge it. Returns the stored path."""
    item_id = item["id"]
    expected_sha = (item.get("sha256") or "").lower()
    expected_size = int(item.get("size") or 0)

    known = store.known(item_id)
    if known:
        # Already stored earlier (the ack may have been lost): re-verify, then ack again.
        path = os.path.join(store.root, known["path"])
        if known["sha256"] == expected_sha and store.verify(item_id, known):
            client.ack(item_id, known["sha256"])
            store.mark_acked(item_id)
            log.info("re-acknowledged %s (already stored)", item_id)
            return path
        log.warning("manifest entry for %s does not match disk; downloading again", item_id)

    final = store.target_path(item)
    if os.path.exists(final):
        if sha256_file(final) == expected_sha:
            store.record(item, final, expected_sha, os.path.getsize(final))
            client.ack(item_id, expected_sha)
            store.mark_acked(item_id)
            return final
        raise TransferError("%s exists with different content; refusing to overwrite" % final)

    os.makedirs(os.path.dirname(final), exist_ok=True)
    part = final + ".part"
    h = hashlib.sha256()
    received = 0
    log.info("downloading %s (%d bytes)", item_id, expected_size)
    try:
        with client.open_download(item_id) as resp, open(part, "wb") as out:
            header_sha = (resp.headers.get("X-Shallot-SHA256") or "").lower()
            length = resp.headers.get("Content-Length")
            if length is None:
                raise TransferError("no Content-Length")
            length = int(length)
            for block in iter(lambda: resp.read(CHUNK), b""):
                out.write(block)
                h.update(block)
                received += len(block)
            out.flush()
            os.fsync(out.fileno())
        actual = h.hexdigest()
        if received != length:
            raise TransferError("short download: %d of %d bytes" % (received, length))
        if verify:
            if expected_size and received != expected_size:
                raise TransferError("size %d != queue size %d" % (received, expected_size))
            if header_sha and header_sha != expected_sha:
                raise TransferError("header sha256 differs from queue listing")
            if actual != expected_sha:
                raise TransferError("sha256 mismatch: got %s expected %s" % (actual, expected_sha))
        os.replace(part, final)
        fsync_dir(os.path.dirname(final))
    except BaseException:
        if os.path.exists(part):
            os.remove(part)
        raise

    store.record(item, final, actual, received)
    store.manifest["archives"][item_id]["contents"] = archive_contents(final)
    client.ack(item_id, actual)
    store.mark_acked(item_id)
    log.info("stored and acknowledged %s -> %s", item_id, final)
    if store.blocks is not None:
        try:
            store.dedup(item_id)
        except (TransferError, OSError) as ex:
            log.warning("block store: %s left as ZIP: %s", item_id, ex)
    return final


def report_manifest(client, store):
    """Tells Shallot what this PC holds (per title). Returns titles reported."""
    store.ensure_contents()
    store.scan_external()
    sent = 0
    for title_id, entries in sorted(store.reports().items()):
        try:
            r = client.post_manifest(title_id, entries)
            sent += 1
            log.debug("manifest %s: %s", title_id, r)
        except (TransferError, urllib.error.URLError, OSError, ValueError) as e:
            log.warning("manifest for %s not sent: %s", title_id, e)
    return sent


def sync_snapshots(client, store):
    """Stores Shallot's snapshot manifests (which archived file versions of a
    game's save objects belong together) as <TITLE>/_snapshots/<id>.json.
    Returns how many were stored. Older Shallot builds without the endpoint
    are skipped quietly."""
    seen = int(store.manifest.get("snapshots_seen", 0))
    stored = 0
    while True:
        try:
            batch = client.snapshots(seen)
        except urllib.error.HTTPError as e:
            if e.code == 404:
                log.debug("this Shallot build has no snapshot feed")
                return stored
            raise
        if not batch:
            break
        for snap in batch:
            sid = int(snap.get("snapshot_id") or 0)
            title = safe_component((snap.get("game") or {}).get("title_id"), "UNKNOWN")
            if sid <= 0 or snap.get("format") != "shallot-snapshot":
                continue
            folder = os.path.join(store.root, title, "_snapshots")
            os.makedirs(folder, exist_ok=True)
            atomic_write_json(os.path.join(folder, "%08d.json" % sid), snap)
            stored += 1
            seen = max(seen, sid)
        store.manifest["snapshots_seen"] = seen
        atomic_write_json(store.manifest_path, store.manifest)
        if len(batch) < 100:
            break
    if stored:
        log.info("stored %d snapshot manifest(s)", stored)
    return stored


def list_snapshots(store, title_id):
    folder = os.path.join(store.root, safe_component(title_id, "UNKNOWN"), "_snapshots")
    names = sorted(os.listdir(folder), reverse=True) if os.path.isdir(folder) else []
    for name in names:
        with open(os.path.join(folder, name), "r", encoding="utf-8") as f:
            snap = json.load(f)
        what = "game" if snap.get("kind") == "title" else "save " + snap.get("object_id", "")
        print("snapshot #%-6s %s  %-12s %-14s %d save object(s), %d file(s)" % (
            snap.get("snapshot_id"), time.strftime("%Y-%m-%d %H:%M", time.localtime(snap.get("created_at") or 0)),
            what, snap.get("reason", ""), snap.get("object_count", 0), snap.get("file_count", 0)))
    return len(names)


# ── Retention: thin out old versions ───────────────────────────────────
#
# Units are Shallot's snapshots (which archived file versions of a game, or of
# one save object, belonged together). Per (title, user, kind, object):
#   - every snapshot younger than keep_all_days is kept,
#   - then the newest snapshot of each calendar day up to daily_days,
#   - then the newest of each ISO week (if weekly), otherwise none,
#   - the newest snapshot of every group is always kept.
# An archive is kept if a kept snapshot references it, if it is younger than
# keep_all_days, if it is the newest archive of its save file, if it was
# placed here by hand (external) or is not acknowledged yet. Archives that
# no snapshot references (made before snapshots existed) are thinned with
# the same day/week rule per save file. Thinned files are moved to
# <backup_dir>/_trash/ and deleted only after trash_days.

def _bucket(ts, now, pol):
    """None = keep unconditionally; otherwise the bucket whose newest member is kept."""
    age = now - ts
    if age < pol["keep_all_days"] * 86400:
        return None
    t = time.localtime(ts)
    if age < pol["daily_days"] * 86400:
        return ("d", time.strftime("%Y-%m-%d", t))
    if pol.get("weekly", True):
        y, w, _ = datetime.date(t.tm_year, t.tm_mon, t.tm_mday).isocalendar()
        return ("w", "%d-%02d" % (y, w))
    return ("drop", ts)


def _thin(items, now, pol):
    """items: [(ts, key)] of one series. Returns the set of keys to keep."""
    keep, seen = set(), set()
    for ts, key in sorted(items, reverse=True):
        if not keep:
            keep.add(key)                      # newest of the series
            b = _bucket(ts, now, pol)
            if b is not None:
                seen.add(b)
            continue
        b = _bucket(ts, now, pol)
        if b is None:
            keep.add(key)
        elif b[0] != "drop" and b not in seen:
            seen.add(b)
            keep.add(key)
    return keep


def load_snapshots(store):
    out = []
    for title in os.listdir(store.root) if os.path.isdir(store.root) else []:
        folder = os.path.join(store.root, title, "_snapshots")
        if not os.path.isdir(folder):
            continue
        for name in os.listdir(folder):
            if not name.endswith(".json"):
                continue
            path = os.path.join(folder, name)
            try:
                with open(path, "r", encoding="utf-8") as f:
                    out.append((path, json.load(f)))
            except (OSError, ValueError):
                log.warning("unreadable snapshot manifest %s (kept)", path)
    return out


def plan_thin_out(store, now=None, pol=None):
    """Returns (snapshot paths to drop, archive keys to drop). Changes nothing."""
    now = time.time() if now is None else now
    pol = dict(DEFAULT_CONFIG["retention"], **(pol or {}))
    snaps = load_snapshots(store)
    groups = {}
    for path, sn in snaps:
        g = ((sn.get("game") or {}).get("title_id"), sn.get("user_id"), sn.get("kind"), sn.get("object_id", ""))
        groups.setdefault(g, []).append((int(sn.get("created_at") or 0), path))
    kept_paths = set()
    for items in groups.values():
        kept_paths |= _thin(items, now, pol)
    needed, referenced = set(), set()
    for path, sn in snaps:
        ids = {m.get("archive_id") for o in sn.get("objects") or [] for m in o.get("members") or [] if m.get("archive_id")}
        referenced |= ids
        if path in kept_paths:
            needed |= ids
    archives = store.manifest["archives"]
    newest = {}
    for key, e in archives.items():
        series = (e.get("title_id"), e.get("user_id"), e.get("save_name"))
        if newest.get(series, (-1, ""))[0] < int(e.get("created_at") or 0):
            newest[series] = (int(e.get("created_at") or 0), key)
    newest_keys = {k for _, k in newest.values()}
    legacy = {}
    drop = []
    for key, e in archives.items():
        if e.get("trashed_at") or e.get("origin") == "external" or not e.get("acked"):
            continue
        ts = int(e.get("created_at") or 0)
        if key not in referenced:             # made before snapshots: thinned per save file
            legacy.setdefault((e.get("title_id"), e.get("user_id"), e.get("save_name")), []).append((ts, key))
            continue
        if key in newest_keys or key in needed or now - ts < pol["keep_all_days"] * 86400:
            continue
        drop.append(key)                      # only in thinned-out snapshots
    for items in legacy.values():
        keep = _thin(items, now, pol)
        drop += [k for _, k in items if k not in keep and k not in newest_keys and k not in needed]
    drop_snaps = [p for p, _ in snaps if p not in kept_paths]
    return drop_snaps, sorted(drop)


def thin_out(store, now=None, pol=None, dry_run=False):
    """Moves thinned snapshots/archives to _trash and purges old trash.
    Returns (snapshots, archives, bytes) moved (or that would be moved)."""
    now = time.time() if now is None else now
    pol = dict(DEFAULT_CONFIG["retention"], **(pol or {}))
    drop_snaps, drop_keys = plan_thin_out(store, now, pol)
    trash = os.path.join(store.root, "_trash")
    freed = 0
    for key in drop_keys:
        e = store.manifest["archives"][key]
        src = os.path.join(store.root, e["path"])
        size = int(e.get("size") or 0) if store.in_blocks(e) else (os.path.getsize(src) if os.path.exists(src) else 0)
        freed += size
        if dry_run:
            log.info("would thin out %s (%s, %d bytes)", key, e.get("save_name"), size)
            continue
        if store.in_blocks(e):
            e["trashed_at"] = int(now)          # blocks stay until the purge below
            continue
        dst = os.path.join(trash, e["path"])
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.exists(src):
            os.replace(src, dst)
        e["trashed_at"] = int(now)
        e["trash_path"] = os.path.relpath(dst, store.root)
    for path in drop_snaps:
        if dry_run:
            log.info("would thin out snapshot %s", os.path.relpath(path, store.root))
            continue
        dst = os.path.join(trash, os.path.relpath(path, store.root))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        os.replace(path, dst)
    if not dry_run:
        # Purge trash older than trash_days (archives tracked in the manifest; snapshot files by mtime).
        for key in list(store.manifest["archives"]):
            e = store.manifest["archives"][key]
            if e.get("trashed_at") and now - e["trashed_at"] >= pol["trash_days"] * 86400:
                if store.in_blocks(e) and store.blocks is not None:
                    store.blocks.delete(key)
                elif e.get("trash_path"):
                    tp = os.path.join(store.root, e["trash_path"])
                    if os.path.isfile(tp):
                        os.remove(tp)
                del store.manifest["archives"][key]
        if os.path.isdir(trash):
            for dirpath, _, files in os.walk(trash):
                for name in files:
                    fp = os.path.join(dirpath, name)
                    if name.endswith(".json") and now - os.path.getmtime(fp) >= pol["trash_days"] * 86400:
                        os.remove(fp)
        store.manifest["last_thin_out"] = int(now)
        atomic_write_json(store.manifest_path, store.manifest)
        if store.blocks is not None:
            reclaimed = store.blocks.gc()
            if reclaimed:
                log.info("block store: reclaimed %.1f MB", reclaimed / 1e6)
    if drop_keys or drop_snaps:
        log.info("%s %d archive(s) (%.1f MB) and %d snapshot manifest(s)%s", "would thin out" if dry_run else "thinned out",
                 len(drop_keys), freed / 1e6, len(drop_snaps), "" if dry_run else " into _trash")
    return len(drop_snaps), len(drop_keys), freed


def find_version(store, ref):
    """Manifest entry for a queue id / "ext:<sha>" key, or for a ZIP path."""
    if ref in store.manifest["archives"]:
        return ref, store.manifest["archives"][ref]
    full = os.path.abspath(ref)
    if os.path.isfile(full):
        sha = sha256_file(full)
        for key, e in store.manifest["archives"].items():
            if e.get("sha256") == sha:
                return key, e
        contents = archive_contents(full)
        return "file:" + sha, {"path": full, "sha256": sha, "user_id": "", "contents": contents,
                               "title_id": contents[0]["title_id"] if contents else ""}
    raise TransferError("no archive %s in the manifest or on disk" % ref)


def list_versions(store, title_id):
    store.ensure_contents()
    rows = []
    for key, e in store.manifest["archives"].items():
        for c in e.get("contents") or []:
            if c["title_id"] == title_id:
                rows.append((int(e.get("created_at") or 0), key, c["save_name"], e.get("origin", "ps5"),
                             c["sha256"][:12], store.present(key, e)))
    for created, key, save, origin, sha, present in sorted(rows, reverse=True):
        print("%s  %-50s %-24s %-8s %s%s" % (time.strftime("%Y-%m-%d %H:%M", time.localtime(created)), key, save,
                                              origin, sha, "" if present else "  (not on this PC)"))
    if not rows:
        print("no versions of %s on this PC" % title_id)


def restore_version(client, store, ref, save=None, user=None, wait_s=300):
    """Uploads one archived version to the PS5 and waits for the result."""
    key, e = find_version(store, ref)
    if e.get("trashed_at"):
        raise TransferError("%s was thinned out (still in the trash: remove its trashed_at in manifest.json to keep it)" % key)
    path, tmp = store.materialize(key, e)
    try:
        return _restore_path(client, e, key, path, save, user, wait_s)
    finally:
        if tmp:
            os.remove(tmp)


def _restore_path(client, e, key, path, save, user, wait_s):
    contents = e.get("contents") or archive_contents(path)
    if save:
        contents = [c for c in contents if c["save_name"] == save]
    if len(contents) != 1:
        raise TransferError("archive holds %d matching saves; pick one with --save" % len(contents))
    c = contents[0]
    user = user or e.get("user_id") or ""
    if not user:
        raise TransferError("archive does not record the PS5 user; pass --user <hex user id>")
    if sha256_file(path) != e["sha256"]:
        raise TransferError("archive on disk does not match its recorded SHA-256")
    log.info("restoring %s %s (user %s) from %s", c["title_id"], c["save_name"], user, key)
    client.upload_restore(path, c["title_id"], c["save_name"], user, e["sha256"], c["sha256"])
    t0 = time.time()
    while time.time() - t0 < wait_s:
        time.sleep(2)
        st = client.restore_status()
        if not st.get("active"):
            if st.get("state") == "done":
                log.info("restore finished: %s", st.get("message"))
                return st
            raise TransferError("restore %s: %s" % (st.get("state"), st.get("message")))
        log.info("restore %s ...", st.get("step") or st.get("state"))
    raise TransferError("restore still running after %d s; check the Backup & Sync tab" % wait_s)


# ── Off-site vault (VPS) ──────────────────────────────────────────────

class VaultError(Exception):
    pass


class VaultCrypto:
    """AES-256-GCM for contents, keyed hashes for names. Both keys come from
    one 32-byte secret kept in vault.key on this PC."""

    def __init__(self, secret):
        try:
            from cryptography.hazmat.primitives.ciphers.aead import AESGCM
        except ImportError:
            raise VaultError("the vault needs the 'cryptography' package: py -m pip install cryptography")
        import hmac as _hmac
        self._hmac = _hmac
        self.enc_key = _hmac.new(secret, b"shallot-vault/enc/v1", hashlib.sha256).digest()
        self.id_key = _hmac.new(secret, b"shallot-vault/id/v1", hashlib.sha256).digest()
        self.aead = AESGCM(self.enc_key)

    def block_id(self, data):
        return self._hmac.new(self.id_key, data, hashlib.sha256).hexdigest()

    def name(self, text):
        return self._hmac.new(self.id_key, b"name:" + text.encode("utf-8"), hashlib.sha256).hexdigest()

    def seal(self, data, aad):
        nonce = os.urandom(12)
        return nonce + self.aead.encrypt(nonce, data, aad.encode("utf-8"))

    def open(self, blob, aad):
        try:
            return self.aead.decrypt(blob[:12], blob[12:], aad.encode("utf-8"))
        except Exception:
            raise VaultError("vault data failed authentication (wrong key or tampered)")


def load_or_create_vault_key(path):
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            key = bytes.fromhex(f.read().strip())
        if len(key) != 32:
            raise VaultError("%s does not hold a 32-byte key" % path)
        return key
    key = os.urandom(32)
    with open(path, "w", encoding="utf-8") as f:
        f.write(key.hex() + "\n")
    log.warning("created the vault key %s -- keep a copy somewhere safe: without it the vault cannot be read", path)
    return key


class VaultClient:
    def __init__(self, vcfg, timeout=60):
        self.base = vcfg["url"].rstrip("/")
        self.token = vcfg.get("token") or ""
        self.timeout = timeout
        # Plain HTTP (the vault's signed port, or a vault on this network): sign
        # each request instead of sending the token. HTTPS: bearer token.
        self.signed = vcfg.get("auth") == "signed" or (vcfg.get("auth") != "bearer" and self.base.startswith("http://"))
        self.prefix = urllib.parse.urlsplit(self.base).path
        self.skew = 0             # the vault's clock minus ours (X-Shallot-Server-Time)

    def _learn_clock(self, headers):
        try:
            server = int((headers or {}).get("X-Shallot-Server-Time") or 0)
        except (TypeError, ValueError):
            return
        if server > 0:
            skew = server - int(time.time())
            self.skew = skew if abs(skew) > 30 else 0

    def _headers(self, method, path, data, ctype):
        if not self.signed:
            return {"Authorization": "Bearer " + self.token, "Content-Type": ctype}
        import hmac
        ts, nonce = str(int(time.time()) + self.skew), os.urandom(16).hex()
        msg = "\n".join((method, self.prefix + path, ts, nonce, hashlib.sha256(data or b"").hexdigest())).encode("utf-8")
        return {"X-Shallot-Time": ts, "X-Shallot-Nonce": nonce, "Content-Type": ctype,
                "X-Shallot-Sig": hmac.new(self.token.encode("utf-8"), msg, hashlib.sha256).hexdigest()}

    def _req(self, method, path, data=None, ctype="application/octet-stream", retried=False):
        req = urllib.request.Request(self.base + path, data=data, method=method,
                                     headers=self._headers(method, path, data, ctype))
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                self._learn_clock(r.headers)
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            if e.code == 401:
                before = self.skew
                self._learn_clock(e.headers)
                if self.signed and not retried and self.skew != before:
                    log.warning("vault: this PC's clock is %d s off the vault's; correcting", self.skew)
                    return self._req(method, path, data, ctype, retried=True)
                raise VaultError("the vault refused the token")
            raise VaultError("vault: HTTP %d on %s" % (e.code, path))

    def _json(self, method, path, obj=None):
        raw = self._req(method, path, json.dumps(obj).encode("utf-8") if obj is not None else None, "application/json")
        return json.loads(raw.decode("utf-8")) if raw else {}

    def ping(self):
        return self._json("GET", "/v1/ping")

    def missing(self, ids):
        out = []
        for i in range(0, len(ids), 4000):
            out += self._json("POST", "/v1/blocks/missing", {"ids": ids[i:i + 4000]}).get("missing", [])
        return out

    def put_block(self, bid, blob):
        self._req("PUT", "/v1/blocks/" + bid, blob)

    def put_blocks(self, records):
        """records: [(id, blob)] uploaded in one request."""
        if records:
            self._req("POST", "/v1/blocks/batch",
                      b"".join(bytes.fromhex(i) + len(b).to_bytes(4, "big") + b for i, b in records))

    def get_blocks(self, ids):
        """{id: blob} for the ids the vault has (one request)."""
        raw = self._req("POST", "/v1/blocks/fetch", json.dumps({"ids": ids}).encode("utf-8"), "application/json") or b""
        out, pos = {}, 0
        while pos + 36 <= len(raw):
            n = int.from_bytes(raw[pos + 32:pos + 36], "big")
            out[raw[pos:pos + 32].hex()] = raw[pos + 36:pos + 36 + n]
            pos += 36 + n
        return out

    def get_block(self, bid):
        b = self._req("GET", "/v1/blocks/" + bid)
        if b is None:
            raise VaultError("block %s is missing on the vault" % bid[:12])
        return b

    def put_meta(self, kind, name, blob):
        self._req("PUT", "/v1/meta/%s/%s" % (kind, name), blob)

    def get_meta(self, kind, name):
        return self._req("GET", "/v1/meta/%s/%s" % (kind, name))

    def delete_meta(self, kind, name):
        self._req("DELETE", "/v1/meta/%s/%s" % (kind, name))

    def list_meta(self, kind):
        return self._json("GET", "/v1/meta/%s/" % kind).get("names", [])

    def all_blocks(self):
        return self._json("GET", "/v1/blocks/").get("ids", [])

    def gc(self, min_age_s=86400):
        """The vault deletes blocks no archive uses (from every client's refs lists)
        and older than min_age_s (an upload in progress writes blocks first)."""
        raw = self._req("POST", "/v1/gc", json.dumps({"min_age_s": int(min_age_s)}).encode("utf-8"), "application/json")
        if raw is None:
            raise VaultError("this vault is too old to clean up safely; update it (vault/ in the Shallot files)")
        return json.loads(raw.decode("utf-8"))

    def delete_blocks(self, ids):
        n = 0
        for i in range(0, len(ids), 4000):
            n += int(self._json("POST", "/v1/blocks/delete", {"ids": ids[i:i + 4000]}).get("deleted", 0))
        return n


def archive_chunks(store, key, e):
    """The archive's bytes in the same blocks the block store uses (first block
    ends where the save image starts, then block_size)."""
    if store.in_blocks(e):
        for b in store.blocks.iter_bytes(key):
            yield b
        return
    path = os.path.join(store.root, e["path"])
    first = _image_offset(path)
    size = 4096 if store.blocks is None else store.blocks.block_size
    with open(path, "rb") as f:
        while True:
            b = f.read(first if first else size)
            first = 0
            if not b:
                break
            yield b


class Vault:
    """One PC store mirrored to a vault. State kept beside the store:
    manifest["vault"][key] = {"at", "name"}, manifest["vault_snaps"][rel] = at,
    and _vault/<key>.ids (the block ids of each uploaded archive, for gc)."""

    def __init__(self, store, vcfg):
        self.store = store
        self.cfg = vcfg
        if vcfg.get("key_hex"):                 # handed over by the PS5 ("PC and VPS", PC route)
            key = bytes.fromhex(vcfg["key_hex"])
            if len(key) != 32:
                raise VaultError("the PS5 sent a vault key of the wrong size")
        else:
            key = load_or_create_vault_key(vcfg["key_file"])
        self.crypto = VaultCrypto(key)
        self.client = VaultClient(vcfg)
        self.ids_dir = os.path.join(store.root, "_vault")
        os.makedirs(self.ids_dir, exist_ok=True)
        # The upload records belong to one vault and key: start over when either changes.
        ident = hashlib.sha256((self.client.base + "\n").encode("utf-8") + key).hexdigest()[:16]
        if store.manifest.get("vault_ident") not in (None, ident):
            log.info("vault: other vault or key than before; everything is checked again")
            store.manifest["vault"], store.manifest["vault_snaps"], store.manifest["vault_reported"] = {}, {}, {}
            for name in os.listdir(self.ids_dir):
                os.remove(os.path.join(self.ids_dir, name))
        if store.manifest.get("vault_ident") != ident:
            store.manifest["vault_ident"] = ident
            atomic_write_json(store.manifest_path, store.manifest)
        self.source = vcfg.get("source", "own")
        self.state = {"state": "starting", "pending": 0, "at": 0, "error": ""}
        self.last_full = 0.0

    def _ids_path(self, key):
        return os.path.join(self.ids_dir, safe_component(key, "x") + ".ids")

    def pending(self):
        done = self.store.manifest.get("vault", {})
        return [k for k, e in self.store.manifest["archives"].items()
                if k not in done and not e.get("trashed_at") and (e.get("acked") or e.get("origin") == "external")]

    def push_archive(self, key):
        e = self.store.manifest["archives"][key]
        ids, h, size = [], hashlib.sha256(), 0
        for b in archive_chunks(self.store, key, e):
            ids.append(self.crypto.block_id(b))
            h.update(b)
            size += len(b)
        if h.hexdigest() != e["sha256"]:
            raise TransferError("%s does not match its SHA-256 on this PC; not uploaded" % key)
        want = set(self.client.missing(sorted(set(ids))))
        sent, batch, batch_bytes = 0, [], 0
        for b in archive_chunks(self.store, key, e):
            bid = self.crypto.block_id(b)
            if bid in want:
                blob = self.crypto.seal(b, bid)
                batch.append((bid, blob))
                batch_bytes += len(blob) + 36
                want.discard(bid)
                sent += len(b)
                if batch_bytes >= 8 * 1024 * 1024:          # ~8 MB per request
                    self.client.put_blocks(batch)
                    batch, batch_bytes = [], 0
        self.client.put_blocks(batch)
        name = self.crypto.name("archive:" + key)
        recipe = {"v": 1, "key": key, "sha256": e["sha256"], "size": size, "ids": ids,
                  "entry": {k: e.get(k) for k in ("title_id", "save_name", "user_id", "created_at", "contents", "path", "origin")}}
        self.client.put_meta("archive", name, self.crypto.seal(json.dumps(recipe).encode("utf-8"), "archive:" + name))
        raw_ids = b"".join(bytes.fromhex(i) for i in ids)
        self.client.put_meta("refs", name, raw_ids)      # block ids in the clear, for the vault's clean-up
        with open(self._ids_path(key), "wb") as f:
            f.write(raw_ids)
        self.store.manifest.setdefault("vault", {})[key] = {"at": int(time.time()), "name": name, "refs": 1}
        return sent

    def push_snapshots(self):
        done = self.store.manifest.setdefault("vault_snaps", {})
        n = 0
        for path, _ in load_snapshots(self.store):
            rel = os.path.relpath(path, self.store.root).replace(os.sep, "/")
            if rel in done:
                continue
            with open(path, "rb") as f:
                data = f.read()
            name = self.crypto.name("snapshot:" + rel)
            blob = json.dumps({"rel": rel, "json": data.decode("utf-8")}).encode("utf-8")
            self.client.put_meta("snapshot", name, self.crypto.seal(blob, "snapshot:" + name))
            done[rel] = int(time.time())
            n += 1
        return n

    def backfill_refs(self, limit=200):
        """Archives uploaded before block lists existed get theirs (from _vault/<key>.ids)."""
        n = 0
        for key, e in list(self.store.manifest.get("vault", {}).items()):
            if e.get("refs") or n >= limit:
                continue
            try:
                with open(self._ids_path(key), "rb") as f:
                    raw = f.read()
            except OSError:
                continue
            self.client.put_meta("refs", e["name"], raw)
            e["refs"] = 1
            n += 1
        return n

    def forget_dropped(self):
        """Archives thinned out on this PC leave the vault too (their blocks: the vault's clean-up)."""
        done = self.store.manifest.get("vault", {})
        n = 0
        for key in [k for k in done if k not in self.store.manifest["archives"]
                    or self.store.manifest["archives"][k].get("trashed_at")]:
            self.client.delete_meta("archive", done[key]["name"])
            self.client.delete_meta("refs", done[key]["name"])
            try:
                os.remove(self._ids_path(key))
            except OSError:
                pass
            del done[key]
            n += 1
        snaps = self.store.manifest.get("vault_snaps", {})
        for rel in [r for r in snaps if not os.path.exists(os.path.join(self.store.root, r))]:
            self.client.delete_meta("snapshot", self.crypto.name("snapshot:" + rel))
            del snaps[rel]
        return n

    def step(self, budget_s=20.0, force=False):
        """Uploads for at most budget_s. Returns True if work is left."""
        t0 = time.time()
        try:
            todo = self.pending()
            if force or todo or time.time() - self.last_full > int(self.cfg.get("interval_min", 10)) * 60:
                self.client.ping()
            sent = archives = 0
            for key in todo:
                if time.time() - t0 > budget_s:
                    break
                sent += self.push_archive(key)
                archives += 1
            if archives:
                atomic_write_json(self.store.manifest_path, self.store.manifest)
                log.info("vault: %d archive(s) uploaded (%.1f MB new after dedup)", archives, sent / 1e6)
            left = len(self.pending())
            if not left and (force or time.time() - self.last_full > int(self.cfg.get("interval_min", 10)) * 60):
                s = self.push_snapshots()
                d = self.forget_dropped()
                r = self.backfill_refs()
                if s or d or r:
                    atomic_write_json(self.store.manifest_path, self.store.manifest)
                if d:
                    try:
                        g = self.client.gc()
                        if g.get("deleted"):
                            log.info("vault: %d unused block(s) deleted", g["deleted"])
                    except VaultError as ex:
                        log.info("vault clean-up skipped: %s", ex)
                self.last_full = time.time()
            self.state = {"state": "pending" if left else "ok", "pending": left, "at": int(time.time()), "error": ""}
            return left > 0
        except (VaultError, TransferError, urllib.error.URLError, OSError, ValueError) as ex:
            if self.state.get("error") != str(ex):
                log.warning("vault: %s", ex)
            self.state = dict(self.state, state="error", error=str(ex)[:200], pending=len(self.pending()))
            return False

    def fetch_archive(self, key, dst):
        """Rebuilds one archive from the vault into dst (verified)."""
        name = self.crypto.name("archive:" + key)
        blob = self.client.get_meta("archive", name)
        if blob is None:
            raise VaultError("the vault has no copy of %s" % key)
        recipe = json.loads(self.crypto.open(blob, "archive:" + name).decode("utf-8"))
        h = hashlib.sha256()
        ids = recipe["ids"]
        with open(dst, "wb") as out:
            for i in range(0, len(ids), 2000):               # ~8 MB per request
                part = ids[i:i + 2000]
                got = self.client.get_blocks(sorted(set(part)))
                for bid in part:
                    if bid not in got:
                        raise VaultError("block %s is missing on the vault" % bid[:12])
                    b = self.crypto.open(got[bid], bid)
                    if self.crypto.block_id(b) != bid:
                        raise VaultError("vault block does not match its name")
                    h.update(b)
                    out.write(b)
            out.flush()
            os.fsync(out.fileno())
        if h.hexdigest() != recipe["sha256"]:
            raise VaultError("rebuilt %s does not match its SHA-256" % key)
        return recipe

    def pull_all(self):
        """Disaster recovery: every archive and snapshot the vault has that this PC lacks."""
        got = 0
        for name in self.client.list_meta("archive"):
            recipe = json.loads(self.crypto.open(self.client.get_meta("archive", name), "archive:" + name).decode("utf-8"))
            key = recipe["key"]
            if self.store.known(key):
                continue
            ent = recipe.get("entry") or {}
            rel = ent.get("path") or os.path.join(safe_component(ent.get("title_id"), "_"), safe_component(ent.get("save_name"), "_"),
                                                  key + ".zip")
            dst = os.path.join(self.store.root, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            tmp = dst + ".part"
            self.fetch_archive(key, tmp)
            os.replace(tmp, dst)
            e = {k: v for k, v in ent.items() if v is not None}
            e.update({"path": os.path.relpath(dst, self.store.root), "sha256": recipe["sha256"], "size": recipe["size"],
                      "acked": True, "origin": ent.get("origin") or "vault"})
            self.store.manifest["archives"][key] = e
            self.store.manifest.setdefault("vault", {})[key] = {"at": int(time.time()), "name": name}
            got += 1
        for name in self.client.list_meta("snapshot"):
            data = json.loads(self.crypto.open(self.client.get_meta("snapshot", name), "snapshot:" + name).decode("utf-8"))
            dst = os.path.join(self.store.root, data["rel"])
            if not os.path.exists(dst):
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, "w", encoding="utf-8") as f:
                    f.write(data["json"])
                self.store.manifest.setdefault("vault_snaps", {})[data["rel"]] = int(time.time())
        atomic_write_json(self.store.manifest_path, self.store.manifest)
        return got

    def gc(self, min_age_s=86400):
        """Has the vault delete blocks no archive uses. The vault decides from every
        client's block lists (a PS5 may upload to the same vault), never from this
        PC's alone. Returns the number deleted."""
        if self.backfill_refs(limit=10 ** 9):
            atomic_write_json(self.store.manifest_path, self.store.manifest)
        g = self.client.gc(min_age_s)
        if g.get("skipped"):
            raise VaultError(g.get("error") or "the vault skipped the clean-up")
        return int(g.get("deleted", 0))


def open_vault(store, cfg):
    v = cfg.get("vault") or {}
    if not v.get("url"):
        return None
    try:
        return Vault(store, v)
    except VaultError as e:
        log.error("vault disabled: %s", e)
        return None


def serve_requests(client, store, vault=None):
    """Sends back the archives Shallot asked for. Each one is rebuilt from this
    PC's store and checked against its SHA-256 before it leaves; Shallot checks
    it again. Returns the number sent, or None if Shallot does not support it."""
    wanted = client.requests()
    if wanted is None:
        return None
    sent = 0
    for w in wanted:
        key = w.get("id")
        e = store.manifest["archives"].get(key)
        tmp = None
        if (not e or e.get("trashed_at")) and vault is not None:
            try:                                  # no longer on this PC: relay it from the vault
                fd, tmp = tempfile.mkstemp(prefix="shallot_send_", suffix=".zip", dir=store.root)
                os.close(fd)
                recipe = vault.fetch_archive(key, tmp)
                if w.get("sha256") and recipe["sha256"] != w["sha256"]:
                    raise VaultError("the vault's copy is a different archive")
                client.readopt(key, tmp)
                sent += 1
                log.info("sent %s back to the PS5 from the vault", key)
            except (VaultError, TransferError, urllib.error.URLError, OSError, ValueError) as ex:
                log.error("could not relay %s from the vault: %s", key, ex)
            finally:
                if tmp and os.path.exists(tmp):
                    os.remove(tmp)
            continue
        if not e or (w.get("sha256") and e.get("sha256") != w["sha256"]):
            log.warning("Shallot asked for %s, which this PC does not hold", key)
            continue
        try:
            if store.in_blocks(e):
                fd, tmp = tempfile.mkstemp(prefix="shallot_send_", suffix=".zip", dir=store.root)
                os.close(fd)
                store.blocks.write_to(key, tmp, e["sha256"])
                path = tmp
            else:
                path = os.path.join(store.root, e["path"])
                if sha256_file(path) != e["sha256"]:
                    raise TransferError("stored copy does not match its SHA-256")
            client.readopt(key, path)
            sent += 1
            log.info("sent %s back to the PS5 (restore requested there)", key)
        except (TransferError, urllib.error.URLError, OSError, ValueError) as ex:
            log.error("could not send %s back to the PS5: %s", key, ex)
        finally:
            if tmp and os.path.exists(tmp):
                os.remove(tmp)
    return sent


def drain(client, store, verify=True):
    """Fetch every pending item once. Returns (stored, failed)."""
    stored = failed = 0
    for item in client.pending():
        try:
            fetch_one(client, store, item, verify)
            stored += 1
        except (TransferError, urllib.error.URLError, OSError, ValueError) as e:
            failed += 1
            log.error("item %s not stored (left queued on PS5): %s", item.get("id"), e)
    return stored, failed


def setup_logging(cfg, verbose):
    log.setLevel(logging.DEBUG if verbose else logging.INFO)
    fmt = logging.Formatter("%(asctime)s %(levelname)s %(message)s")
    console = logging.StreamHandler()
    console.setFormatter(fmt)
    log.addHandler(console)
    if cfg.get("log_file"):
        fh = logging.handlers.RotatingFileHandler(cfg["log_file"], maxBytes=1_000_000,
                                                  backupCount=3, encoding="utf-8")
        fh.setFormatter(fmt)
        log.addHandler(fh)


def export_version(store, ref, dst):
    """Writes one stored archive back out as a normal ZIP (exact original bytes)."""
    key, e = find_version(store, ref)
    if store.in_blocks(e):
        store.blocks.write_to(key, dst, e["sha256"])
    else:
        src = os.path.join(store.root, e["path"])
        with open(src, "rb") as f, open(dst, "wb") as out:
            for b in iter(lambda: f.read(CHUNK), b""):
                out.write(b)
        if sha256_file(dst) != e["sha256"]:
            raise TransferError("exported copy does not match its SHA-256")
    log.info("exported %s -> %s", key, dst)


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="Pull Shallot SaveMgr backups to this PC")
    ap.add_argument("--config", default=os.path.join(here, "config.json"))
    ap.add_argument("--once", action="store_true", help="drain the queue once and exit")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--list", metavar="TITLE_ID", help="list the versions this PC holds for a title")
    ap.add_argument("--restore", metavar="ID_OR_ZIP", help="restore one archived version to the PS5")
    ap.add_argument("--save", help="save name inside the archive (title-wide archives)")
    ap.add_argument("--user", help="PS5 user id (hex) for archives that do not record it")
    ap.add_argument("--prune", action="store_true", help="thin out old versions now (retention settings in config)")
    ap.add_argument("--dry-run", action="store_true", help="with --prune: only list what would be thinned out")
    ap.add_argument("--dedup", action="store_true", help="move every stored archive into the block store now")
    ap.add_argument("--export", nargs=2, metavar=("ID", "ZIP"), help="write one archive back out as a normal ZIP")
    ap.add_argument("--tray", action="store_true", help="run with a tray icon (pip install pystray pillow)")
    ap.add_argument("--vault-pull", action="store_true", help="rebuild this PC's store from the VPS vault")
    ap.add_argument("--vault-gc", action="store_true", help="delete vault blocks no archive needs any more")
    ap.add_argument("--autostart", choices=("on", "off"), help="start the tray client with Windows, or stop doing so")
    args = ap.parse_args(argv)
    if args.autostart:
        p = set_autostart(args.autostart == "on", args.config)
        print("autostart %s: %s" % (args.autostart, p))
        if args.autostart == "on" and hasattr(os, "startfile"):
            os.startfile(p)               # start the tray client now as well
            print("tray client started (look for the shallot icon in the taskbar tray)")
        return 0

    cfg = load_config(args.config)
    setup_logging(cfg, args.verbose)
    client = ShallotClient(cfg)
    store = Store(cfg["backup_dir"], dedup=cfg.get("dedup") or {})
    client.cid = store.client_id()
    global ALLOW_REMOTE_UPDATE
    ALLOW_REMOTE_UPDATE = bool(cfg.get("allow_remote_update", False))
    verify = bool(cfg.get("verify_downloads", True))
    log.info("Shallot sync: %s -> %s", client.base, store.root)
    if args.list:
        if list_snapshots(store, args.list):
            print()
        list_versions(store, args.list)
        return 0
    if args.export:
        try:
            export_version(store, args.export[0], args.export[1])
            return 0
        except (TransferError, OSError) as e:
            log.error("export failed: %s", e)
            return 1
    if args.dedup:
        store.ensure_contents()
        store.dedup_pending()
        return 0
    retention = dict(DEFAULT_CONFIG["retention"], **(cfg.get("retention") or {}))
    if args.prune:
        thin_out(store, pol=retention, dry_run=args.dry_run)
        return 0
    if args.restore:
        try:
            restore_version(client, store, args.restore, args.save, args.user)
            return 0
        except (TransferError, urllib.error.URLError, OSError, ValueError) as e:
            log.error("restore failed: %s", e)
            return 1

    vault = open_vault(store, cfg)
    if args.vault_pull or args.vault_gc:
        if vault is None:
            log.error("no vault configured (\"vault\": {\"url\": ..., \"token\": ...} in config.json)")
            return 1
        try:
            if args.vault_pull:
                log.info("vault: %d archive(s) restored to this PC", vault.pull_all())
            if args.vault_gc:
                log.info("vault: %d unused block(s) deleted", vault.gc())
            return 0
        except (VaultError, urllib.error.URLError, OSError, ValueError) as e:
            log.error("vault: %s", e)
            return 1
    if args.tray:
        rc = run_tray(client, store, cfg, verify, retention, args.config, vault)
    else:
        rc = run_forever(client, store, verify, retention, once=args.once, vault=vault)
    if rc == RESTART:                      # the web UI installed a new version of this script
        log.info("starting the updated client")
        if store.blocks is not None:
            store.blocks.close()
        os.execv(sys.executable, [sys.executable, os.path.abspath(__file__)] + sys.argv[1:])
    return rc


class SyncState:
    """What the sync loop is doing, for the tray icon (thread-safe enough: plain fields)."""

    def __init__(self, kind=None):
        self.online = None          # None = not tried yet
        self.backlog = 0
        self.last_stored = 0.0
        self.stored_total = 0
        self.failed_last = 0
        self.error = ""
        self.wake = threading.Event()
        self.stop = threading.Event()
        self.serve_requests = True
        self.vault = None
        self.paused = False         # collecting paused from the web UI
        self.standby = False        # another PC client is the active one: do nothing
        self.vault_acks = True
        self.restart = False        # "update": start the new script after stopping
        self.kind = kind or ("docker" if os.environ.get("SHALLOT_CONTAINER") else "console")


# Commands the web UI can send (POST /api/pc/command on the PS5). A container
# is restarted by Docker and its script lives in the image: no quit/update.
COMMANDS = ("ping", "sync", "pause", "resume", "thin", "vault", "update", "quit")
# "update" replaces this script with one sent by the PS5. Someone posing as the
# PS5 on the network could send their own code, so it needs a yes on this PC
# (the tray on Windows asks) unless config.json says "allow_remote_update": true.
ALLOW_REMOTE_UPDATE = False


def can_confirm_on_pc(kind):
    return kind == "tray" and os.name == "nt"


def client_caps(kind):
    caps = [c for c in COMMANDS if not (kind == "docker" and c in ("update", "quit"))]
    if "update" in caps and not (ALLOW_REMOTE_UPDATE or can_confirm_on_pc(kind)):
        caps.remove("update")
    return caps


def ask_on_pc(title, text):
    """Yes/No box on this PC (Windows); False when nobody can be asked."""
    if os.name != "nt":
        return False
    import ctypes
    MB_YESNO, MB_ICONQUESTION, MB_TOPMOST, MB_SETFOREGROUND, IDYES = 0x4, 0x20, 0x40000, 0x10000, 6
    return ctypes.windll.user32.MessageBoxW(None, text, title, MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDYES


def store_info(store, state):
    held = size = 0
    for e in store.manifest["archives"].values():
        if not e.get("trashed_at"):
            held += 1
            size += int(e.get("size") or 0)
    try:
        free = shutil.disk_usage(store.root).free
    except OSError:
        free = 0
    return {"kind": state.kind, "caps": ",".join(client_caps(state.kind)), "paused": "1" if state.paused else "0",
            "held": str(held), "held_bytes": str(size), "free": str(free)}


def fetch_client(client):
    """The sync client bundled with Shallot: (script bytes, its version). Checked to parse."""
    raw = client.client_script()
    m = re.search(rb'CLIENT_VERSION = "([^"]+)"', raw)
    if not m:
        raise TransferError("Shallot did not send a sync client")
    compile(raw, "shallot_sync.py", "exec")           # never install something that does not even parse
    return raw, m.group(1).decode("ascii", "replace")


def install_client(raw):
    here = os.path.abspath(__file__)
    tmp = here + ".new"
    with open(tmp, "wb") as f:
        f.write(raw)
        f.flush()
        os.fsync(f.fileno())
    if os.path.exists(here):
        shutil.copy2(here, here + ".bak")
    os.replace(tmp, here)


def update_client(client):
    """Replaces this script with the one bundled in Shallot. Returns the new version."""
    raw, version = fetch_client(client)
    install_client(raw)
    return version


def run_command(client, store, state, holder, retention, cmd):
    """Runs one web UI command. Returns (ok, short result text)."""
    if cmd not in client_caps(state.kind):
        return False, "not supported by this client (%s)" % state.kind
    if cmd == "ping":
        return True, "pong from %s (%s, v%s)" % (socket.gethostname()[:40], state.kind, CLIENT_VERSION)
    if cmd == "sync":
        state.wake.set()
        return True, "collecting now"
    if cmd in ("pause", "resume"):
        state.paused = cmd == "pause"
        store.manifest["paused"] = state.paused
        atomic_write_json(store.manifest_path, store.manifest)
        log.info("collecting %s from the web UI", "paused" if state.paused else "resumed")
        return True, "paused" if state.paused else "collecting again"
    if cmd == "thin":
        snaps, keys, freed = thin_out(store, pol=dict(retention, enabled=True))
        return True, "%d old version(s) and %d restore point(s) thinned out (%.1f MB)" % (keys, snaps, freed / 1e6)
    if cmd == "vault":
        v = holder.get("vault")
        if v is None:
            return False, "no VPS copy is set up on this PC"
        v.step(60, force=True)
        state.vault = v.state
        return v.state.get("state") != "error", "VPS copy: %s%s" % (v.state.get("state"),
                                                                  (" - " + v.state["error"]) if v.state.get("error") else "")
    if cmd == "update":
        raw, new = fetch_client(client)
        if new == CLIENT_VERSION:
            return True, "already v%s" % new
        if not ALLOW_REMOTE_UPDATE:
            digest = hashlib.sha256(raw).hexdigest()

            def ask():
                text = ("The Shallot web UI asks to update this sync client from v%s to v%s.\n\n"
                        "Only say yes if you just pressed Update client yourself.\n\nSHA-256 of the new script:\n%s"
                        % (CLIENT_VERSION, new, digest))
                if ask_on_pc("Shallot sync: update?", text):
                    install_client(raw)
                    log.info("updated to v%s after confirmation on this PC", new)
                    state.restart = True
                    state.stop.set()
                    state.wake.set()
                else:
                    log.warning("update to v%s declined on this PC", new)

            threading.Thread(target=ask, daemon=True).start()
            return True, "asked on the PC: confirm the update there"
        install_client(raw)
        state.restart = True
        state.stop.set()
        return True, "updated to v%s, restarting" % new
    if cmd == "quit":
        state.stop.set()
        return True, "quitting"
    return False, "unknown command"


def report_vault_acks(client, store):
    """Tells Shallot which of its archives this PC has put in the vault, so the
    PS5 keeps each one until both the PC and the vault have it."""
    done = store.manifest.setdefault("vault_reported", {})
    todo = [k for k in store.manifest.get("vault", {}) if k not in done and not k.startswith("file:")]
    for i in range(0, len(todo), 400):
        batch = todo[i:i + 400]
        if not client.vault_acks(batch):
            return None                      # older Shallot
        for k in batch:
            done[k] = 1
    if todo:
        atomic_write_json(store.manifest_path, store.manifest)
    return len(todo)


def refresh_relay(client, store, holder):
    """"PC and VPS" with the PC uploading: use the PS5's vault settings, unless
    config.json names a vault of its own."""
    if holder.get("own"):
        return
    try:
        rc = client.vault_config()
    except (urllib.error.URLError, OSError, TransferError, ValueError):
        return
    if rc is None:
        return
    want = (rc.get("url"), rc.get("token"), rc.get("key")) if rc.get("route") == "pc" and rc.get("url") else None
    if want == holder.get("relay"):
        return
    holder["relay"] = want
    if want is None:
        if holder.get("vault") is not None:
            log.info("vault: the PS5 no longer asks this PC to upload")
        holder["vault"] = None
        client.vault_state = None
        return
    try:
        holder["vault"] = Vault(store, dict(DEFAULT_CONFIG["vault"], url=want[0], token=want[1], key_hex=want[2], source="ps5"))
        log.info("vault: uploading for the PS5 to %s", want[0])
    except (VaultError, ValueError, OSError) as e:
        log.error("vault settings from the PS5 not usable: %s", e)
        holder["vault"] = None


def run_forever(client, store, verify, retention, once=False, state=None, vault=None):
    """Takes everything Shallot has queued, then waits for the next archive.
    Anything not stored stays queued on the PS5 and is retried on the next pass.
    Returns 0, or RESTART after the web UI installed a new version."""
    state = state or SyncState()
    state.paused = bool(store.manifest.get("paused"))
    holder = {"vault": vault, "own": vault is not None, "relay": None}
    offline = False
    last_report = 0.0
    last_relay = 0.0
    seq = 0
    backoff = 5
    while not state.stop.is_set():
        force_report = False
        try:
            if time.time() - last_relay > 600:
                refresh_relay(client, store, holder)
                last_relay = time.time()
            vault = holder["vault"]
            idle = state.paused or state.standby
            stored, failed = drain(client, store, verify) if not idle else (0, 0)
            state.online, state.error, state.failed_last = True, "", failed
            if offline:
                log.info("PS5 reachable again")
                offline = False
                last_report = 0.0
            backoff = 5
            if stored:
                state.stored_total += stored
                state.last_stored = time.time()
            if stored or failed:
                log.info("pass done: %d stored, %d failed", stored, failed)
            if state.serve_requests and not state.standby:
                if serve_requests(client, store, vault) is None:
                    state.serve_requests = False     # older Shallot
            vault_busy = False
            if vault is not None and not idle:
                vault_busy = vault.step(float(vault.cfg.get("slice_s", 20)), force=bool(stored))
                if state.vault_acks and report_vault_acks(client, store) is None:
                    state.vault_acks = False         # older Shallot
            if vault is not None:
                state.vault = dict(vault.state, src=vault.source)
                client.vault_state = state.vault
            if not state.standby and (stored or once or time.time() - last_report > 600):
                sync_snapshots(client, store)
            if not idle:
                if retention.get("enabled") and time.time() - int(store.manifest.get("last_thin_out", 0)) > retention["interval_h"] * 3600:
                    thin_out(store, pol=retention)
                store.dedup_pending(limit=20)      # existing ZIPs move into the block store gradually
            # Report holdings after new archives, at start, when asked and every 10 minutes.
            if not state.standby and (stored or once or time.time() - last_report > 600):
                n = report_manifest(client, store)
                last_report = time.time()
                if n:
                    log.info("reported holdings for %d title(s) to Shallot", n)
            if once:
                return 0
            if failed:                         # retry soon, but do not spin
                state.wake.wait(10)
                state.wake.clear()
                continue
            # Wait for the next archive (or "Sync now"); old Shallot builds: plain polling.
            w = client.wait(seq, timeout=1 if vault_busy else WAIT_S, info=store_info(store, state))
            if w is None:
                state.wake.wait(max(5, int(client.cfg.get("poll_interval_s", 30))))
                state.wake.clear()
                continue
            state.backlog = int(w.get("pending", 0))
            if bool(w.get("standby")) != state.standby:
                state.standby = bool(w.get("standby"))
                log.info("another PC client is the active one: standing by" if state.standby
                         else "this PC client collects the backups now")
            if w.get("reason") == "kick":
                log.info("sync requested")
                force_report = True
            if w.get("reason") == "config":
                last_relay = 0.0
            seq = int(w["seq"])
            if w.get("cmd"):
                try:
                    ok, result = run_command(client, store, state, holder, retention, w["cmd"])
                except (urllib.error.URLError, OSError, TransferError, ValueError, SyntaxError, VaultError) as e:
                    ok, result = False, str(e)
                log.info("web UI command %s: %s", w["cmd"], result)
                client.command_done(w.get("cmd_id", 0), ok, result)
                if w["cmd"] == "sync":
                    force_report = True
        except (urllib.error.URLError, OSError, TransferError, ValueError) as e:
            state.online, state.error = False, str(e)
            if not offline:  # log once per outage, not every retry
                log.warning("PS5 not reachable: %s", e)
                offline = True
            if once:
                return 0
            state.wake.wait(backoff)
            state.wake.clear()
            backoff = min(backoff * 2, max(5, int(client.cfg.get("poll_interval_s", 30))))
        except Exception as e:      # never let the sync thread die silently (pythonw has no console)
            log.exception("sync pass failed; retrying in 30 s")
            state.error = "internal error: %s" % e
            state.failed_last = max(state.failed_last, 1)
            if once:
                return 1
            state.wake.wait(30)
            state.wake.clear()
        if force_report:
            last_report = 0.0
    return RESTART if state.restart else 0


# ── Windows autostart and tray ────────────────────────────────────────

def startup_script_path():
    appdata = os.environ.get("APPDATA")
    if not appdata:
        return None
    return os.path.join(appdata, "Microsoft", "Windows", "Start Menu", "Programs", "Startup", "ShallotSync.vbs")


def autostart_enabled():
    p = startup_script_path()
    return bool(p and os.path.exists(p))


def set_autostart(on, config_path):
    """Starts the tray client at login (a hidden .vbs launcher in the Startup folder)."""
    p = startup_script_path()
    if not p:
        raise OSError("autostart is only supported on Windows")
    if not on:
        if os.path.exists(p):
            os.remove(p)
        return p
    exe = sys.executable
    pyw = os.path.join(os.path.dirname(exe), "pythonw.exe")
    if os.path.exists(pyw):
        exe = pyw
    cmd = '"%s" "%s" --tray --config "%s"' % (exe, os.path.abspath(__file__), os.path.abspath(config_path))
    with open(p, "w", encoding="utf-8") as f:
        f.write('CreateObject("WScript.Shell").Run "%s", 0, False\n' % cmd.replace('"', '""'))
    return p


def tray_image(color):
    from PIL import Image, ImageDraw
    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse((6, 10, 58, 60), fill=(178, 100, 106, 255), outline=(96, 50, 58, 255), width=3)
    d.line((32, 4, 32, 14), fill=(110, 140, 70, 255), width=5)
    d.ellipse((40, 40, 62, 62), fill=color, outline=(30, 30, 30, 255), width=2)
    return img


def run_tray(client, store, cfg, verify, retention, config_path, vault=None):
    try:
        import pystray
        from pystray import MenuItem as Item
        tray_image((128, 128, 128, 255))
    except ImportError:
        log.warning("tray icon needs: pip install pystray pillow -- running without it")
        return run_forever(client, store, verify, retention, vault=vault)
    import webbrowser

    state = SyncState(kind="tray")
    result = {}

    def work():
        result["rc"] = run_forever(client, store, verify, retention, state=state, vault=vault)

    worker = threading.Thread(target=work, daemon=True)
    worker.start()
    colors = {"ok": (60, 170, 80, 255), "busy": (230, 160, 30, 255), "off": (200, 60, 50, 255), "init": (128, 128, 128, 255)}

    def mood():
        if state.online is None:
            return "init", "Shallot sync: connecting to %s" % client.base
        if not state.online:
            return "off", "Shallot sync: PS5 not reachable"
        if state.failed_last or state.backlog:
            return "busy", "Shallot sync: %d archive(s) waiting" % max(state.backlog, state.failed_last)
        when = time.strftime("%H:%M", time.localtime(state.last_stored)) if state.last_stored else "-"
        if state.paused:
            return "busy", "Shallot sync: paused from the web UI"
        if state.standby:
            return "busy", "Shallot sync: standing by (another PC client collects; switch in the web UI)"
        v = state.vault or {}
        if v.get("state") == "error":
            return "busy", "Shallot sync: up to date; VPS copy failing"
        if v.get("state") == "pending":
            return "ok", "Shallot sync: up to date; VPS copy: %d archive(s) to upload" % v.get("pending", 0)
        return "ok", "Shallot sync: up to date (last archive %s)" % when

    def sync_now(icon, item):
        try:
            client.kick()
        except (urllib.error.URLError, OSError, TransferError, ValueError):
            pass
        state.wake.set()

    def toggle_autostart(icon, item):
        try:
            set_autostart(not autostart_enabled(), config_path)
        except OSError as e:
            log.error("autostart: %s", e)

    def quit_(icon, item):
        state.stop.set()
        state.wake.set()
        icon.stop()

    menu = pystray.Menu(
        Item(lambda item: mood()[1], None, enabled=False),
        Item("Sync now", sync_now, default=True),
        Item("Open Shallot web UI", lambda i, it: webbrowser.open(client.base + "/")),
        Item("Open backup folder", lambda i, it: os.startfile(store.root) if hasattr(os, "startfile") else None),
        Item("Start with Windows", toggle_autostart, checked=lambda item: autostart_enabled()),
        Item("Quit", quit_))
    icon = pystray.Icon("shallot_sync", tray_image(colors["init"]), "Shallot sync", menu)

    def refresh():
        last = None
        while not state.stop.is_set():
            m, text = mood()
            if m != last:
                icon.icon = tray_image(colors[m])
                last = m
            icon.title = text[:127]
            time.sleep(2)
        icon.stop()                        # quit or update from the web UI

    threading.Thread(target=refresh, daemon=True).start()
    icon.run()
    worker.join(timeout=30)
    return result.get("rc", 0)

if __name__ == "__main__":
    sys.exit(main())
