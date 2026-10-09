"""
Tests for pc/shallot_sync.py against a fake Shallot /api/backup/* server.
Run: python -m unittest discover -s tests/pc -v   (from the repo root)
"""
import hashlib
import http.server
import json
import os
import shutil
import sys
import tempfile
import threading
import unittest
import urllib.parse
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "pc"))
import shallot_sync  # noqa: E402


class FakeShallot:
    """Mimics the PS5 queue: items stay pending until acked with the right sha."""

    def __init__(self):
        self.items = {}       # id -> dict(row, data)
        self.acks = []
        self.corrupt = set()  # ids served with flipped bytes
        self.truncate = set() # ids whose body is cut short
        self.fail_ack_once = set()
        self.manifests = {}   # title -> list of entries last posted
        self.restores = []    # (params, body sha256) received
        self.snaps = []       # snapshot manifests served by /api/backup/snapshots (None = old build, 404)
        fake = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _json(self, obj, code=200):
                body = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                u = urllib.parse.urlparse(self.path)
                q = dict(urllib.parse.parse_qsl(u.query))
                if u.path == "/api/restore/status":
                    return self._json({"restore": {"active": False, "state": "done", "message": "restored gen 7"}})
                if u.path == "/api/backup/snapshots":
                    if fake.snaps is None:
                        return self._json({"error": "Not found"}, 404)
                    since = int(q.get("since") or 0)
                    return self._json({"snapshots": [s for s in fake.snaps if s["snapshot_id"] > since][:100]})
                if u.path == "/api/backup/queue":
                    rows = [it["row"] for it in fake.items.values() if not it["row"]["lan_ack_at"]]
                    return self._json({"queue": rows})
                if u.path == "/api/backup/queue_download":
                    it = fake.items.get(q.get("id"))
                    if not it:
                        return self._json({"error": "Queue item not found"}, 404)
                    data = it["data"]
                    if q["id"] in fake.corrupt:
                        data = bytes([data[0] ^ 0xFF]) + data[1:]
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(it["data"])))
                    self.send_header("X-Shallot-SHA256", it["row"]["sha256"])
                    self.end_headers()
                    if q["id"] in fake.truncate:
                        self.wfile.write(data[: len(data) // 2])
                        self.close_connection = True
                        return
                    self.wfile.write(data)
                    return
                self._json({"error": "Not found"}, 404)

            def do_POST(self):
                u = urllib.parse.urlparse(self.path)
                q = dict(urllib.parse.parse_qsl(u.query))
                if u.path == "/api/restore/upload":
                    n = int(self.headers.get("Content-Length") or 0)
                    body = self.rfile.read(n)
                    if hashlib.sha256(body).hexdigest() != q.get("archive_sha256"):
                        return self._json({"error": "Uploaded archive does not match archive_sha256"})
                    fake.restores.append((q, hashlib.sha256(body).hexdigest()))
                    return self._json({"ok": True, "queued": True})
                if u.path == "/api/backup/pc_manifest":
                    n = int(self.headers.get("Content-Length") or 0)
                    body = json.loads(self.rfile.read(n).decode("utf-8"))
                    fake.manifests[q["title"]] = body["archives"]
                    return self._json({"ok": True, "stored": len(body["archives"])})
                if u.path == "/api/backup/queue_ack":
                    it = fake.items.get(q.get("id"))
                    if not it:
                        return self._json({"error": "Queue item not found"})
                    if q.get("id") in fake.fail_ack_once:
                        fake.fail_ack_once.discard(q["id"])
                        return self._json({"error": "simulated ack failure"})
                    if q.get("sha256") != it["row"]["sha256"]:
                        return self._json({"error": "sha256 mismatch"})
                    it["row"]["lan_ack_at"] = 1
                    fake.acks.append(q["id"])
                    return self._json({"ok": True})
                self._json({"error": "Not found"}, 404)

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def add(self, item_id, data, title="PPSA01234", save="SAVE0000"):
        self.items[item_id] = {"data": data, "row": {
            "id": item_id, "title_id": title, "save_name": save, "user_id": "10000000",
            "sha256": hashlib.sha256(data).hexdigest(), "size": len(data),
            "created_at": 1791000000, "reason": "game_close", "lan_ack_at": 0}}

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class SyncTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="shallot-sync-test-")
        self.fake = FakeShallot()
        cfg = dict(shallot_sync.DEFAULT_CONFIG, ps5_host="127.0.0.1", port=self.fake.port,
                   backup_dir=self.tmp, http_timeout_s=5, log_file=None)
        self.client = shallot_sync.ShallotClient(cfg)
        self.store = shallot_sync.Store(self.tmp)

    def tearDown(self):
        self.fake.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def stored_files(self):
        out = []
        for root, _, files in os.walk(self.tmp):
            out += [os.path.join(root, f) for f in files if f.endswith(".zip")]
        return out

    def no_part_files(self):
        for root, _, files in os.walk(self.tmp):
            if any(f.endswith(".part") for f in files):
                return False
        return True

    def test_normal_download_store_ack(self):
        self.fake.add("q1", b"PK" + os.urandom(3000))
        stored, failed = shallot_sync.drain(self.client, self.store)
        self.assertEqual((stored, failed), (1, 0))
        self.assertEqual(self.fake.acks, ["q1"])
        files = self.stored_files()
        self.assertEqual(len(files), 1)
        self.assertIn(os.path.join("PPSA01234", "SAVE0000"), files[0])
        with open(files[0], "rb") as f:
            self.assertEqual(f.read(), self.fake.items["q1"]["data"])
        entry = self.store.known("q1")
        self.assertTrue(entry["acked"])
        # Manifest survives a restart of the client.
        self.assertTrue(shallot_sync.Store(self.tmp).known("q1")["acked"])
        # Nothing left pending, second pass does nothing.
        self.assertEqual(shallot_sync.drain(self.client, self.store), (0, 0))

    def test_corrupt_download_is_not_acked(self):
        self.fake.add("bad", b"PK" + os.urandom(2000))
        self.fake.corrupt.add("bad")
        self.assertEqual(shallot_sync.drain(self.client, self.store), (0, 1))
        self.assertEqual(self.fake.acks, [])
        self.assertEqual(self.stored_files(), [])
        self.assertTrue(self.no_part_files())
        self.fake.corrupt.clear()
        self.assertEqual(shallot_sync.drain(self.client, self.store), (1, 0))
        self.assertEqual(self.fake.acks, ["bad"])

    def test_truncated_download_is_not_acked(self):
        self.fake.add("cut", b"PK" + os.urandom(50000))
        self.fake.truncate.add("cut")
        self.assertEqual(shallot_sync.drain(self.client, self.store), (0, 1))
        self.assertEqual(self.fake.acks, [])
        self.assertTrue(self.no_part_files())

    def test_lost_ack_is_retried_without_redownload(self):
        self.fake.add("q2", b"PK-data-2")
        self.fake.fail_ack_once.add("q2")
        self.assertEqual(shallot_sync.drain(self.client, self.store), (0, 1))
        self.assertEqual(len(self.stored_files()), 1)       # stored durably
        self.assertFalse(self.store.known("q2")["acked"])
        self.assertEqual(shallot_sync.drain(self.client, self.store), (1, 0))
        self.assertEqual(self.fake.acks, ["q2"])
        self.assertEqual(len(self.stored_files()), 1)       # not duplicated

    def test_never_overwrites_different_content(self):
        self.fake.add("q3", b"PK-original")
        path = self.store.target_path(self.fake.items["q3"]["row"])
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(b"something else")
        self.assertEqual(shallot_sync.drain(self.client, self.store), (0, 1))
        with open(path, "rb") as f:
            self.assertEqual(f.read(), b"something else")
        self.assertEqual(self.fake.acks, [])

    def test_unsafe_names_are_sanitized(self):
        self.fake.add("q4", b"PK-x", title="PPSA0/../..", save='a:b*c?"')
        self.assertEqual(shallot_sync.drain(self.client, self.store), (1, 0))
        for f in self.stored_files():
            self.assertTrue(os.path.abspath(f).startswith(os.path.abspath(self.tmp)))

    @staticmethod
    def make_zip(entries):
        import io
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as z:
            for name, data in entries:
                z.writestr(name, data)
        return buf.getvalue()

    def test_manifest_reports_container_hashes(self):
        img = os.urandom(5000)
        self.fake.add("q10", self.make_zip([("PPSA01234/sdimg_SAVE0", img)]), save="sdimg_SAVE0")
        shallot_sync.drain(self.client, self.store)
        self.assertEqual(shallot_sync.report_manifest(self.client, self.store), 1)
        m = self.fake.manifests["PPSA01234"]
        self.assertEqual(len(m), 1)
        self.assertEqual(m[0]["queue_id"], "q10")
        self.assertEqual(m[0]["save_name"], "sdimg_SAVE0")
        self.assertEqual(m[0]["container_sha256"], hashlib.sha256(img).hexdigest())
        self.assertEqual(m[0]["archive_sha256"], self.fake.items["q10"]["row"]["sha256"])
        self.assertEqual(m[0]["origin"], "ps5")

    def test_ps4_key_file_is_not_a_save(self):
        z = self.make_zip([("CUSA00001/sdimg_SAVE", b"img"), ("CUSA00001/SAVE.bin", b"key")])
        path = os.path.join(self.tmp, "x.zip")
        with open(path, "wb") as f:
            f.write(z)
        c = shallot_sync.archive_contents(path)
        self.assertEqual([x["save_name"] for x in c], ["sdimg_SAVE"])
        # Digimon-style image names ending in .bin are saves.
        z = self.make_zip([("PPSA24701/sdimg_0000.bin", b"img")])
        with open(path, "wb") as f:
            f.write(z)
        self.assertEqual([x["save_name"] for x in shallot_sync.archive_contents(path)], ["sdimg_0000.bin"])

    def test_external_archives_are_reported(self):
        d = os.path.join(self.tmp, "manual")
        os.makedirs(d)
        img = b"from-another-console"
        with open(os.path.join(d, "other.zip"), "wb") as f:
            f.write(self.make_zip([("PPSA05555/sdimg_SLOT", img)]))
        with open(os.path.join(d, "notes.zip"), "wb") as f:
            f.write(b"not a zip")
        shallot_sync.report_manifest(self.client, self.store)
        m = self.fake.manifests["PPSA05555"]
        self.assertEqual(len(m), 1)
        self.assertEqual(m[0]["origin"], "external")
        self.assertEqual(m[0]["queue_id"], "")
        self.assertEqual(m[0]["container_sha256"], hashlib.sha256(img).hexdigest())
        # Registered once, not duplicated on the next report.
        shallot_sync.report_manifest(self.client, self.store)
        self.assertEqual(len(self.fake.manifests["PPSA05555"]), 1)

    def test_old_entries_backfilled_and_deleted_files_dropped(self):
        img = b"legacy"
        self.fake.add("q11", self.make_zip([("PPSA01234/sdimg_OLD", img)]), save="sdimg_OLD")
        shallot_sync.drain(self.client, self.store)
        del self.store.manifest["archives"]["q11"]["contents"]       # as written by an older client
        shallot_sync.report_manifest(self.client, self.store)
        self.assertEqual(self.fake.manifests["PPSA01234"][0]["container_sha256"], hashlib.sha256(img).hexdigest())
        os.remove(os.path.join(self.tmp, self.store.manifest["archives"]["q11"]["path"]))
        self.fake.manifests.clear()
        shallot_sync.report_manifest(self.client, self.store)
        self.assertEqual(self.fake.manifests.get("PPSA01234"), [],
                         "title is reported with an empty list so Shallot forgets the removed archive")

    def test_restore_uploads_the_chosen_version(self):
        img = b"older-save-bytes"
        self.fake.add("q20", self.make_zip([("PPSA01234/sdimg_SLOT", img)]), save="sdimg_SLOT")
        shallot_sync.drain(self.client, self.store)
        st = shallot_sync.restore_version(self.client, self.store, "q20", wait_s=10)
        self.assertEqual(st["state"], "done")
        q, sha = self.fake.restores[-1]
        self.assertEqual(q["title"], "PPSA01234")
        self.assertEqual(q["save"], "sdimg_SLOT")
        self.assertEqual(q["user"], "10000000")
        self.assertEqual(q["container_sha256"], hashlib.sha256(img).hexdigest())
        self.assertEqual(sha, self.store.known("q20")["sha256"])

    def test_restore_external_needs_user_and_single_save(self):
        d = os.path.join(self.tmp, "ext")
        os.makedirs(d)
        zp = os.path.join(d, "two.zip")
        with open(zp, "wb") as f:
            f.write(self.make_zip([("PPSA05555/sdimg_A", b"a"), ("PPSA05555/sdimg_B", b"b")]))
        with self.assertRaises(shallot_sync.TransferError):
            shallot_sync.restore_version(self.client, self.store, zp, wait_s=5)          # two saves
        with self.assertRaises(shallot_sync.TransferError):
            shallot_sync.restore_version(self.client, self.store, zp, save="sdimg_A", wait_s=5)   # no user
        shallot_sync.restore_version(self.client, self.store, zp, save="sdimg_A", user="1a2b3c4d", wait_s=5)
        self.assertEqual(self.fake.restores[-1][0]["save"], "sdimg_A")
        self.assertEqual(self.fake.restores[-1][0]["user"], "1a2b3c4d")

    def test_restore_refuses_tampered_archive(self):
        self.fake.add("q21", self.make_zip([("PPSA01234/sdimg_T", b"x")]), save="sdimg_T")
        shallot_sync.drain(self.client, self.store)
        path = os.path.join(self.tmp, self.store.known("q21")["path"])
        with open(path, "ab") as f:
            f.write(b"tamper")
        with self.assertRaises(shallot_sync.TransferError):
            shallot_sync.restore_version(self.client, self.store, "q21", wait_s=5)
        self.assertEqual(self.fake.restores, [])

    def test_snapshot_manifests_are_stored_once(self):
        def snap(i, title="PPSA01234"):
            return {"format": "shallot-snapshot", "version": 1, "snapshot_id": i, "kind": "title", "object_id": "",
                    "game": {"title_id": title, "name": "Game"}, "user_id": "10000000", "created_at": 1791000000 + i,
                    "reason": "game_close", "object_count": 1, "file_count": 2,
                    "objects": [{"id": "Save04", "members": [
                        {"role": "primary", "file": "sdimg_Save04", "gen_id": 3, "sha256": "aa", "archive_id": "q1"},
                        {"role": "sce_backup", "file": "sdimg_sce_bu_Save04", "gen_id": 4, "sha256": "bb", "archive_id": "q2"}]}]}
        self.fake.snaps = [snap(1), snap(2, "../evil"), snap(3)]
        self.assertEqual(shallot_sync.sync_snapshots(self.client, self.store), 3)
        f = os.path.join(self.tmp, "PPSA01234", "_snapshots", "00000003.json")
        with open(f, encoding="utf-8") as fh:
            self.assertEqual(json.load(fh)["objects"][0]["members"][1]["file"], "sdimg_sce_bu_Save04")
        for root, _, files in os.walk(self.tmp):
            self.assertTrue(os.path.abspath(root).startswith(os.path.abspath(self.tmp)))
        # Already seen: nothing again; restart keeps the marker.
        self.assertEqual(shallot_sync.sync_snapshots(self.client, shallot_sync.Store(self.tmp)), 0)
        self.fake.snaps.append(snap(7))
        self.assertEqual(shallot_sync.sync_snapshots(self.client, self.store), 1)
        self.assertEqual(shallot_sync.list_snapshots(self.store, "PPSA01234"), 3)

    def test_old_shallot_without_snapshot_feed(self):
        self.fake.snaps = None
        self.assertEqual(shallot_sync.sync_snapshots(self.client, self.store), 0)

    def test_ps5_offline_raises_without_damage(self):
        self.fake.stop()
        with self.assertRaises(Exception):
            shallot_sync.drain(self.client, self.store)
        self.assertEqual(self.stored_files(), [])
        self.fake = FakeShallot()  # tearDown needs something to stop


if __name__ == "__main__":
    unittest.main()
