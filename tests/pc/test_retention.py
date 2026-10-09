"""
Tests for the PC-side thinning of old versions (pc/shallot_sync.py thin_out).
Run: python -m unittest discover -s tests/pc -v   (from the repo root)
"""
import json
import os
import shutil
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "pc"))
import shallot_sync  # noqa: E402

DAY = 86400
NOW = time.mktime((2026, 10, 7, 12, 0, 0, 0, 0, -1))
POL = {"keep_all_days": 2, "daily_days": 30, "weekly": True, "trash_days": 14}


class RetentionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="shallot-ret-test-")
        self.store = shallot_sync.Store(self.tmp)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def archive(self, key, ts, save="sdimg_Save04", title="PPSA01234", acked=True, origin=None):
        rel = os.path.join(title, save, key + ".zip")
        full = os.path.join(self.tmp, rel)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(b"PK" + key.encode())
        e = {"path": rel, "sha256": "x", "size": 10, "title_id": title, "save_name": save, "user_id": "u1",
             "created_at": int(ts), "acked": acked}
        if origin:
            e["origin"] = origin
        self.store.manifest["archives"][key] = e
        return full

    def snapshot(self, sid, ts, archive_ids, title="PPSA01234", kind="title", obj=""):
        folder = os.path.join(self.tmp, title, "_snapshots")
        os.makedirs(folder, exist_ok=True)
        snap = {"format": "shallot-snapshot", "version": 1, "snapshot_id": sid, "kind": kind, "object_id": obj,
                "game": {"title_id": title}, "user_id": "u1", "created_at": int(ts),
                "objects": [{"id": "Save04", "members": [{"role": "primary", "archive_id": a} for a in archive_ids]}]}
        path = os.path.join(folder, "%08d.json" % sid)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(snap, f)
        return path

    def test_buckets(self):
        # one snapshot every 6 hours for 60 days, each with its own archive
        for i in range(240):
            ts = NOW - i * 6 * 3600 - 60
            self.archive("a%03d" % i, ts)
            self.snapshot(i + 1, ts, ["a%03d" % i])
        drop_snaps, drop_keys = shallot_sync.plan_thin_out(self.store, NOW, POL)
        kept = 240 - len(drop_snaps)
        # 2 days * 4 = 8 young ones, then ~1 per day up to day 30 (28), then ~1 per week (~5)
        self.assertGreaterEqual(kept, 8 + 27 + 4)
        self.assertLessEqual(kept, 8 + 29 + 6)
        self.assertEqual(len(drop_keys), len(drop_snaps), "each thinned snapshot frees exactly its archive")
        for i in range(8):
            self.assertNotIn("a%03d" % i, drop_keys, "versions younger than keep_all_days are never thinned")

    def test_archive_needed_by_kept_snapshot_survives(self):
        old = NOW - 40 * DAY
        self.archive("shared", old)                      # unchanged slot, referenced by every snapshot
        for i in range(10):
            ts = NOW - 10 * DAY - i * 3600
            self.archive("v%d" % i, ts)
            self.snapshot(i + 1, ts, ["shared", "v%d" % i])
        _, drop = shallot_sync.plan_thin_out(self.store, NOW, POL)
        self.assertNotIn("shared", drop)
        self.assertIn("v1", drop)                         # same day as v0, older
        self.assertNotIn("v0", drop)                      # newest of that day

    def test_external_unacked_and_newest_are_never_thinned(self):
        self.archive("ext", NOW - 90 * DAY, origin="external")
        self.archive("pending", NOW - 90 * DAY, save="sdimg_Save05", acked=False)
        self.archive("only", NOW - 90 * DAY, save="sdimg_Save06")
        self.archive("old1", NOW - 90 * DAY, save="sdimg_Save07")
        self.archive("old2", NOW - 90 * DAY + 60, save="sdimg_Save07")
        _, drop = shallot_sync.plan_thin_out(self.store, NOW, POL)
        self.assertEqual(drop, ["old1"], "legacy archives thinned per save file, newest kept")

    def test_trash_then_purge_and_dry_run(self):
        for i in range(3):
            ts = NOW - 10 * DAY - i * 60
            self.archive("k%d" % i, ts)
            self.snapshot(i + 1, ts, ["k%d" % i])
        before = sorted(os.listdir(os.path.join(self.tmp, "PPSA01234", "sdimg_Save04")))
        self.assertEqual(shallot_sync.thin_out(self.store, NOW, POL, dry_run=True)[1], 2)
        self.assertEqual(sorted(os.listdir(os.path.join(self.tmp, "PPSA01234", "sdimg_Save04"))), before, "dry run moves nothing")
        n_snaps, n_arch, _ = shallot_sync.thin_out(self.store, NOW, POL)
        self.assertEqual((n_snaps, n_arch), (2, 2))
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "_trash", "PPSA01234", "sdimg_Save04", "k1.zip")))
        self.assertTrue(self.store.manifest["archives"]["k1"]["trashed_at"])
        # Trashed archives are neither reported to Shallot nor re-registered as external.
        self.assertEqual(self.store.scan_external(), 0)
        for entries in self.store.reports().values():
            self.assertNotIn("k1", [x["queue_id"] for x in entries])
        # After trash_days the files are gone for good.
        shallot_sync.thin_out(self.store, NOW + 15 * DAY, POL)
        self.assertNotIn("k1", self.store.manifest["archives"])
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "_trash", "PPSA01234", "sdimg_Save04", "k1.zip")))
        self.assertIn("k0", self.store.manifest["archives"])

    def test_weekly_off_drops_older_than_daily(self):
        for i in range(5):
            ts = NOW - (40 + i * 7) * DAY
            self.archive("w%d" % i, ts)
            self.snapshot(i + 1, ts, ["w%d" % i])
        drop_snaps, drop = shallot_sync.plan_thin_out(self.store, NOW, dict(POL, weekly=False))
        self.assertEqual(len(drop_snaps), 4, "only the newest snapshot of the game survives")
        self.assertEqual(sorted(drop), ["w1", "w2", "w3", "w4"])


if __name__ == "__main__":
    unittest.main()
