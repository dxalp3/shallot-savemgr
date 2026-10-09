"""
Tests for the PC block store (pc/shallot_sync.py BlockStore / Store.dedup).
Run: python -m unittest discover -s tests/pc -v   (from the repo root)
"""
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import time
import unittest
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "pc"))
import shallot_sync  # noqa: E402
from test_shallot_sync import FakeShallot  # noqa: E402


def save_image(seed, n_blocks=512, changed=()):
    """A fake 'sdimg' image: deterministic random blocks, a few changed ones."""
    out = bytearray()
    for i in range(n_blocks):
        tag = b"v%d" % seed if i in changed else b"base"
        out += hashlib.sha256(tag + i.to_bytes(4, "little")).digest() * 128     # 4096 bytes
    return bytes(out)


def zip_bytes(image, title="PPSA01234", save="sdimg_Save04"):
    bio = io.BytesIO()
    with zipfile.ZipFile(bio, "w", zipfile.ZIP_STORED) as z:
        z.writestr("%s/%s" % (title, save), image)
    return bio.getvalue()


class BlockTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="shallot-blk-test-")
        self.store = shallot_sync.Store(self.tmp, dedup={})

    def tearDown(self):
        if self.store.blocks:
            self.store.blocks.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def add_zip(self, key, data, ts=None, save="sdimg_Save04"):
        rel = os.path.join("PPSA01234", save, key + ".zip")
        full = os.path.join(self.tmp, rel)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(data)
        self.store.manifest["archives"][key] = {
            "path": rel, "sha256": hashlib.sha256(data).hexdigest(), "size": len(data), "title_id": "PPSA01234",
            "save_name": save, "user_id": "u1", "created_at": int(ts or time.time()), "acked": True}
        return full

    def test_roundtrip_and_dedup(self):
        v1 = zip_bytes(save_image(1))
        v2 = zip_bytes(save_image(2, changed={3, 200, 511}))      # 3 blocks differ, like Atomfall
        p1 = self.add_zip("a1", v1)
        self.add_zip("a2", v2)
        w1 = self.store.dedup("a1")
        w2 = self.store.dedup("a2")
        self.assertFalse(os.path.exists(p1), "ZIP removed after a verified move")
        self.assertGreater(w1, len(v1) // 2)
        # 3 changed image blocks + the ZIP header and directory (blocks are aligned to the image).
        self.assertLessEqual(w2, 3 * 4096 + 400, "second version costs only its changed blocks (%d bytes)" % w2)
        st = self.store.blocks.stats()
        self.assertEqual(st["archives"], 2)
        self.assertLess(st["pack_bytes"], len(v1) + 3 * 4096 + 400)
        out = os.path.join(self.tmp, "x.zip")
        shallot_sync.export_version(self.store, "a2", out)
        with open(out, "rb") as f:
            self.assertEqual(f.read(), v2, "exact original bytes")
        e = self.store.manifest["archives"]["a2"]
        self.assertEqual(e["stored"], "blocks")
        self.assertEqual(e["contents"][0]["save_name"], "sdimg_Save04", "contents cached before the ZIP went away")
        # Reopen from disk: state survives.
        self.store.blocks.close()
        self.store = shallot_sync.Store(self.tmp, dedup={})
        self.assertTrue(self.store.verify("a1", self.store.manifest["archives"]["a1"]) if "a1" in self.store.manifest["archives"]
                        else self.store.blocks.sha_of("a1") == hashlib.sha256(v1).hexdigest())

    def test_damaged_pack_is_detected(self):
        data = zip_bytes(save_image(1))
        self.add_zip("a1", data)
        self.store.dedup("a1")
        pack = [f for f in os.listdir(os.path.join(self.tmp, "_blocks")) if f.startswith("pack-")][0]
        path = os.path.join(self.tmp, "_blocks", pack)
        with open(path, "r+b") as f:
            f.seek(5000)
            f.write(b"\xff")
        for f in self.store.blocks._files.values():
            f.close()
        self.store.blocks._files.clear()
        self.assertFalse(self.store.verify("a1", self.store.manifest["archives"]["a1"]))
        with self.assertRaises(shallot_sync.TransferError):
            shallot_sync.export_version(self.store, "a1", os.path.join(self.tmp, "y.zip"))
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "y.zip")), "no partial output")

    def test_external_and_unacked_stay_zip(self):
        p = self.add_zip("ext", zip_bytes(save_image(5)))
        self.store.manifest["archives"]["ext"]["origin"] = "external"
        q = self.add_zip("pend", zip_bytes(save_image(6)), save="sdimg_Save05")
        self.store.manifest["archives"]["pend"]["acked"] = False
        self.assertEqual(self.store.dedup_pending(), 0)
        self.assertTrue(os.path.exists(p) and os.path.exists(q))

    def test_interrupted_move_is_finished(self):
        data = zip_bytes(save_image(1))
        p = self.add_zip("a1", data)
        # Simulate: blocks + manifest written, ZIP not yet deleted.
        self.store.blocks.put_file("a1", p, hashlib.sha256(data).hexdigest())
        self.store.manifest["archives"]["a1"]["stored"] = "blocks"
        self.store.dedup_pending()
        self.assertFalse(os.path.exists(p))
        self.assertTrue(self.store.verify("a1", self.store.manifest["archives"]["a1"]))

    def test_thin_out_purge_reclaims_blocks(self):
        day = 86400
        now = time.time()
        for i in range(6):
            self.add_zip("v%d" % i, zip_bytes(save_image(i, changed={i})), ts=now - 40 * day - i * 3600)
        self.store.dedup_pending()
        before = self.store.blocks.stats()
        pol = {"keep_all_days": 2, "daily_days": 30, "weekly": True, "trash_days": 14}
        n_snaps, n_arch, _ = shallot_sync.thin_out(self.store, now, pol)
        self.assertEqual(n_arch, 5, "same week: only the newest survives")
        self.assertTrue(self.store.blocks.has("v1"), "trashed versions keep their blocks until the purge")
        shallot_sync.thin_out(self.store, now + 15 * day, pol)
        self.assertFalse(self.store.blocks.has("v1"))
        after = self.store.blocks.stats()
        self.assertLess(after["blocks"], before["blocks"])
        self.assertEqual(self.store.blocks.sha_of("v0"), self.store.manifest["archives"]["v0"]["sha256"],
                         "survivor intact after garbage collection / compaction")


class ClientFlowTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="shallot-blk-flow-")
        self.fake = FakeShallot()
        cfg = dict(shallot_sync.DEFAULT_CONFIG, ps5_host="127.0.0.1", port=self.fake.port, backup_dir=self.tmp,
                   http_timeout_s=5, log_file=None)
        self.client = shallot_sync.ShallotClient(cfg)
        self.store = shallot_sync.Store(self.tmp, dedup={})

    def tearDown(self):
        self.fake.stop()
        self.store.blocks.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_download_dedup_report_and_restore(self):
        v1 = zip_bytes(save_image(1))
        v2 = zip_bytes(save_image(2, changed={9}))
        self.fake.add("q1", v1, save="sdimg_Save04")
        self.fake.add("q2", v2, save="sdimg_Save04")
        self.assertEqual(shallot_sync.drain(self.client, self.store), (2, 0))
        self.assertEqual(sorted(self.fake.acks), ["q1", "q2"])
        zips = [f for _, _, fs in os.walk(self.tmp) for f in fs if f.endswith(".zip")]
        self.assertEqual(zips, [], "both archives live in the block store only")
        # Still reported to Shallot as held.
        rep = self.store.reports()["PPSA01234"]
        self.assertEqual(sorted(x["queue_id"] for x in rep), ["q1", "q2"])
        # A lost ack is re-acknowledged from the block copy (no re-download).
        self.fake.items["q2"]["row"]["lan_ack_at"] = 0
        self.assertEqual(shallot_sync.drain(self.client, self.store), (1, 0))
        # Restore uploads the exact original bytes, rebuilt from blocks.
        shallot_sync.restore_version(self.client, self.store, "q1", wait_s=5)
        self.assertEqual(self.fake.restores[-1][1], hashlib.sha256(v1).hexdigest())


if __name__ == "__main__":
    unittest.main()
