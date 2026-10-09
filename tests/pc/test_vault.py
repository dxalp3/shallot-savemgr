"""
End-to-end tests of the off-site vault: the real server (vault/shallot_vault.py)
on localhost and the PC client's Vault (pc/shallot_sync.py) with real
AES-256-GCM. Needs the 'cryptography' package.
Run: python -m unittest discover -s tests/pc -v   (from the repo root)
"""
import hashlib
import json
import os
import shutil
import sys
import tempfile
import threading
import unittest
import zipfile

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "..", "pc"))
sys.path.insert(0, os.path.join(HERE, "..", "..", "vault"))
import shallot_sync  # noqa: E402
import shallot_vault  # noqa: E402

try:
    import cryptography  # noqa: F401
    HAVE_CRYPTO = True
except ImportError:
    HAVE_CRYPTO = False

TOKEN = "t" * 32
MARKER = b"PLAINTEXT-MARKER-0123456789"


def make_zip(path, image):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as z:
        z.writestr("PPSA01234/sdimg_Save04", image)
    return shallot_sync.sha256_file(path)


@unittest.skipUnless(HAVE_CRYPTO, "needs the cryptography package")
class VaultTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="shallot-vault-test-")
        self.srv = shallot_vault.serve(os.path.join(self.tmp, "srv"), TOKEN, host="127.0.0.1", port=0)
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.store = shallot_sync.Store(os.path.join(self.tmp, "pc"), dedup={"enabled": True})
        self.vcfg = dict(shallot_sync.DEFAULT_CONFIG["vault"], url="http://127.0.0.1:%d" % self.port, token=TOKEN,
                         key_file=os.path.join(self.tmp, "vault.key"))
        # two versions of one save, differing in one 4 KiB block; both contain a plaintext marker
        img = bytearray(os.urandom(200 * 1024))
        img[50000:50000 + len(MARKER)] = MARKER
        self.images = []
        for i in range(2):
            if i:
                img[120000] ^= 0xFF
            key = "a%d" % i
            z = os.path.join(self.store.root, "PPSA01234", "sdimg_Save04", key + ".zip")
            sha = make_zip(z, bytes(img))
            self.store.manifest["archives"][key] = {"path": os.path.relpath(z, self.store.root), "sha256": sha,
                                                    "size": os.path.getsize(z), "title_id": "PPSA01234",
                                                    "save_name": "sdimg_Save04", "user_id": "u1", "created_at": 100 + i,
                                                    "acked": True}
            self.images.append((key, sha))
        self.store.dedup("a0")          # one in the block store, one still a ZIP
        snapdir = os.path.join(self.store.root, "PPSA01234", "_snapshots")
        os.makedirs(snapdir)
        with open(os.path.join(snapdir, "00000001.json"), "w", encoding="utf-8") as f:
            json.dump({"format": "shallot-snapshot", "snapshot_id": 1, "created_at": 100, "game": {"title_id": "PPSA01234"},
                       "objects": []}, f)

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        if self.store.blocks is not None:
            self.store.blocks.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def server_bytes(self):
        out = b""
        for dp, _, files in os.walk(os.path.join(self.tmp, "srv")):
            for f in files:
                with open(os.path.join(dp, f), "rb") as fh:
                    out += fh.read()
        return out

    def test_upload_dedup_encrypted_and_rebuild(self):
        v = shallot_sync.Vault(self.store, self.vcfg)
        self.assertFalse(v.step(budget_s=60, force=True))
        self.assertEqual(v.state["state"], "ok")
        self.assertEqual(set(self.store.manifest["vault"]), {"a0", "a1"})
        stats = v.client._json("GET", "/v1/stats")
        size = self.store.manifest["archives"]["a0"]["size"]
        # second version added about one new block, not a whole copy
        self.assertLess(stats["block_bytes"], size + 4 * (4096 + 28) + 64 * 1024)
        self.assertEqual(stats["meta"]["archive"], 2)
        self.assertEqual(stats["meta"]["snapshot"], 1)
        self.assertNotIn(MARKER, self.server_bytes())          # the VPS sees ciphertext only
        for key, sha in self.images:
            dst = os.path.join(self.tmp, key + ".out")
            v.fetch_archive(key, dst)
            self.assertEqual(shallot_sync.sha256_file(dst), sha)
        self.assertFalse(v.step(budget_s=60))                 # nothing left
        with open(self.vcfg["key_file"], encoding="utf-8") as f:
            self.assertEqual(len(bytes.fromhex(f.read().strip())), 32)

    def test_pull_all_rebuilds_a_lost_pc(self):
        shallot_sync.Vault(self.store, self.vcfg).step(budget_s=60, force=True)
        fresh = shallot_sync.Store(os.path.join(self.tmp, "pc2"), dedup={"enabled": True})
        v2 = shallot_sync.Vault(fresh, self.vcfg)               # same key file
        self.assertEqual(v2.pull_all(), 2)
        for key, sha in self.images:
            e = fresh.manifest["archives"][key]
            self.assertEqual(shallot_sync.sha256_file(os.path.join(fresh.root, e["path"])), sha)
        self.assertTrue(os.path.exists(os.path.join(fresh.root, "PPSA01234", "_snapshots", "00000001.json")))
        if fresh.blocks is not None:
            fresh.blocks.close()

    def test_wrong_key_or_token(self):
        shallot_sync.Vault(self.store, self.vcfg).step(budget_s=60, force=True)
        other = dict(self.vcfg, key_file=os.path.join(self.tmp, "other.key"))
        v = shallot_sync.Vault(self.store, other)
        with self.assertRaises(shallot_sync.VaultError):
            v.fetch_archive("a1", os.path.join(self.tmp, "x"))   # names differ under another key
        bad = shallot_sync.Vault(self.store, dict(self.vcfg, token="wrong" * 6))
        self.store.manifest["vault"] = {}
        bad.step(budget_s=5, force=True)
        self.assertEqual(bad.state["state"], "error")
        self.assertIn("token", bad.state["error"])

    def test_dropped_archives_leave_and_gc(self):
        v = shallot_sync.Vault(self.store, self.vcfg)
        v.step(budget_s=60, force=True)
        before = v.client._json("GET", "/v1/stats")["blocks"]
        del self.store.manifest["archives"]["a1"]
        v.last_full = 0
        v.step(budget_s=60)
        self.assertNotIn("a1", self.store.manifest["vault"])
        self.assertEqual(v.client._json("GET", "/v1/stats")["meta"]["archive"], 1)
        self.assertGreaterEqual(v.gc(min_age_s=0), 1)                     # the block only a1 used
        self.assertLess(v.client._json("GET", "/v1/stats")["blocks"], before)
        dst = os.path.join(self.tmp, "a0.out")
        v.fetch_archive("a0", dst)                             # the rest still rebuilds
        self.assertEqual(shallot_sync.sha256_file(dst), self.images[0][1])

    def test_get_from_pc_relays_from_the_vault(self):
        v = shallot_sync.Vault(self.store, self.vcfg)
        v.step(budget_s=60, force=True)
        sha = self.images[1][1]
        self.store.manifest["archives"]["a1"]["trashed_at"] = 1    # thinned out on this PC
        sent = {}

        class C:
            def requests(self):
                return [{"id": "a1", "sha256": sha}]

            def readopt(self, key, path):
                sent[key] = shallot_sync.sha256_file(path)
                return {"ok": True}

        self.assertEqual(shallot_sync.serve_requests(C(), self.store, v), 1)
        self.assertEqual(sent, {"a1": sha})


@unittest.skipUnless(HAVE_CRYPTO, "needs the cryptography package")
class PcSignedClientTests(unittest.TestCase):
    """The PC client on the signed-only port (no HTTPS needed) and with the PS5's key."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="shallot-pcsigned-test-")
        self.vault = shallot_vault.Vault(os.path.join(self.tmp, "srv"), TOKEN)
        self.srv = shallot_vault.serve(None, TOKEN, host="127.0.0.1", port=0, signed_only=True, vault=self.vault)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.url = "http://127.0.0.1:%d" % self.srv.server_address[1]

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_signed_upload_and_relay_key(self):
        store = shallot_sync.Store(os.path.join(self.tmp, "pc"), dedup={"enabled": False})
        z = os.path.join(store.root, "PPSA01234", "sdimg_Save04", "a0.zip")
        sha = make_zip(z, os.urandom(50000))
        store.manifest["archives"]["a0"] = {"path": os.path.relpath(z, store.root), "sha256": sha, "size": os.path.getsize(z),
                                            "title_id": "PPSA01234", "save_name": "sdimg_Save04", "created_at": 1, "acked": True}
        key = os.urandom(32).hex()
        vcfg = dict(shallot_sync.DEFAULT_CONFIG["vault"], url=self.url, token=TOKEN, key_hex=key, source="ps5",
                    key_file=os.path.join(self.tmp, "should-not-exist.key"))
        v = shallot_sync.Vault(store, vcfg)
        self.assertTrue(v.client.signed)
        self.assertFalse(v.step(budget_s=30, force=True))
        self.assertEqual(v.state["state"], "ok", v.state)
        self.assertFalse(os.path.exists(vcfg["key_file"]))
        dst = os.path.join(self.tmp, "back.zip")
        v.fetch_archive("a0", dst)
        self.assertEqual(shallot_sync.sha256_file(dst), sha)
        # another key: the upload records start over
        v2 = shallot_sync.Vault(store, dict(vcfg, key_hex=os.urandom(32).hex()))
        self.assertEqual(store.manifest["vault"], {})
        self.assertEqual(v2.pending(), ["a0"])
        # bearer is refused on this port
        bad = shallot_sync.VaultClient(dict(url=self.url, token=TOKEN, auth="bearer"))
        with self.assertRaises(shallot_sync.VaultError):
            bad.ping()


@unittest.skipUnless(HAVE_CRYPTO, "needs the cryptography package")
class ClockAndCleanupTests(unittest.TestCase):
    """A client with a wrong clock corrects itself; the vault cleans up only what no archive uses."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="shallot-clock-test-")
        self.vault = shallot_vault.Vault(os.path.join(self.tmp, "srv"), TOKEN)
        self.srv = shallot_vault.serve(None, TOKEN, host="127.0.0.1", port=0, signed_only=True, vault=self.vault)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.url = "http://127.0.0.1:%d" % self.srv.server_address[1]

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_wrong_clock_is_corrected(self):
        c = shallot_sync.VaultClient(dict(url=self.url, token=TOKEN))
        c.skew = -3600                       # as if this PC were an hour behind
        self.assertTrue(c.ping().get("ok"))  # refused once, corrected from the vault's time, retried
        self.assertEqual(c.skew, 0)

    def test_cleanup_needs_every_block_list_and_spares_others(self):
        c = shallot_sync.VaultClient(dict(url=self.url, token=TOKEN))
        a, b, shared = "aa" * 32, "bb" * 32, "cc" * 32
        c.put_blocks([(a, b"1"), (b, b"2"), (shared, b"3")])
        n1, n2 = "11" * 32, "22" * 32
        c.put_meta("archive", n1, b"x")
        c.put_meta("archive", n2, b"y")
        c.put_meta("refs", n1, bytes.fromhex(a + shared))
        g = c.gc(min_age_s=0)
        self.assertTrue(g.get("skipped"))    # n2 has no block list: nothing is deleted
        self.assertEqual(c._json("GET", "/v1/stats")["blocks"], 3)
        c.put_meta("refs", n2, bytes.fromhex(shared))     # e.g. another client's archive
        self.assertEqual(c.gc(min_age_s=0)["deleted"], 1)  # only b, used by nobody
        self.assertEqual(set(c.missing([a, b, shared])), {b})
        c.delete_meta("archive", n1)                       # n1 thinned out (its refs left behind)
        self.assertEqual(c.gc(min_age_s=0)["deleted"], 1)  # a goes, shared stays for n2
        self.assertEqual(set(c.missing([a, shared])), {a})
        self.assertEqual(c.gc(min_age_s=3600)["deleted"], 0)


class SignedPortTests(unittest.TestCase):
    """The PS5's route: plain HTTP, signed requests only, no bearer token."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="shallot-signed-test-")
        self.vault = shallot_vault.Vault(os.path.join(self.tmp, "srv"), TOKEN)
        self.srv = shallot_vault.serve(None, TOKEN, host="127.0.0.1", port=0, signed_only=True, vault=self.vault)
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def call(self, path, headers):
        import urllib.request
        import urllib.error
        req = urllib.request.Request("http://127.0.0.1:%d%s" % (self.port, path), headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.status
        except urllib.error.HTTPError as e:
            return e.code

    def signed(self, path, body=b"", ts=None, nonce=None, token=TOKEN):
        import hmac as _h
        import time as _t
        ts = str(int(_t.time())) if ts is None else ts
        nonce = nonce or os.urandom(16).hex()
        msg = "\n".join(("GET", path, ts, nonce, hashlib.sha256(body).hexdigest())).encode()
        sig = _h.new(token.encode(), msg, hashlib.sha256).hexdigest()
        return {"X-Shallot-Time": ts, "X-Shallot-Nonce": nonce, "X-Shallot-Sig": sig}

    def test_rules(self):
        h = self.signed("/v1/ping")
        self.assertEqual(self.call("/v1/ping", h), 200)
        self.assertEqual(self.call("/v1/ping", h), 401)                                   # replay
        self.assertEqual(self.call("/v1/ping", {"Authorization": "Bearer " + TOKEN}), 401)  # bearer refused here
        self.assertEqual(self.call("/v1/ping", self.signed("/v1/ping", token="x" * 30)), 401)
        self.assertEqual(self.call("/v1/ping", self.signed("/v1/ping", ts="1000")), 401)  # stale
        self.assertEqual(self.call("/v1/stats", self.signed("/v1/ping")), 401)            # signed for another path


if __name__ == "__main__":
    unittest.main()
