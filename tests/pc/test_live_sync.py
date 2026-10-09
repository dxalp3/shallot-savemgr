"""
Tests for the PC client's live loop (pc/shallot_sync.py run_forever) against a
fake Shallot, and for the Windows autostart launcher.
Run: python -m unittest discover -s tests/pc -v   (from the repo root)
"""
import os
import shutil
import sys
import tempfile
import threading
import unittest
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "pc"))
import shallot_sync  # noqa: E402

POL = {"enabled": False}


class FakeShallot:
    """Scripted /api/backup/wait answers; drain() and reports are stubbed out."""

    def __init__(self, waits):
        self.cfg = {"poll_interval_s": 5}
        self.waits = list(waits)
        self.wait_calls = []
        self.infos = []
        self.done = []
        self.relay = None
        self.drains = 0

    def requests(self):
        return []

    def vault_config(self):
        return self.relay

    def command_done(self, cmd_id, ok, result):
        self.done.append((cmd_id, ok, result))

    def wait(self, seq, timeout=shallot_sync.WAIT_S, info=None):
        self.wait_calls.append(seq)
        self.infos.append(info)
        if not self.waits:
            raise AssertionError("unexpected wait")
        w = self.waits.pop(0)
        if isinstance(w, Exception):
            raise w
        return w


class LiveLoopTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="shallot-live-test-")
        self.store = shallot_sync.Store(self.tmp)
        self.orig = (shallot_sync.drain, shallot_sync.sync_snapshots, shallot_sync.report_manifest)
        self.reports = 0
        self.drain_results = []

        def drain(client, store, verify=True):
            client.drains += 1
            return self.drain_results.pop(0) if self.drain_results else (0, 0)

        def report(client, store):
            self.reports += 1
            return 0

        shallot_sync.drain = drain
        shallot_sync.sync_snapshots = lambda c, s: None
        shallot_sync.report_manifest = report

    def tearDown(self):
        shallot_sync.drain, shallot_sync.sync_snapshots, shallot_sync.report_manifest = self.orig
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_loop(self, client, state, stop_after_waits):
        # Stop the loop once the scripted waits are used up.
        orig_wait = client.wait

        def wait(seq, timeout=shallot_sync.WAIT_S, info=None):
            r = orig_wait(seq, timeout, info)
            if len(client.wait_calls) >= stop_after_waits:
                state.stop.set()
            return r

        client.wait = wait
        shallot_sync.run_forever(client, self.store, False, POL, state=state)

    def test_waits_with_the_last_sequence_and_drains_after_each_wake(self):
        client = FakeShallot([{"seq": 7, "reason": "start", "pending": 0},
                             {"seq": 8, "reason": "new", "pending": 1},
                             {"seq": 9, "reason": "timeout", "pending": 0}])
        state = shallot_sync.SyncState()
        self.drain_results = [(0, 0), (1, 0), (0, 0)]
        self.run_loop(client, state, 3)
        self.assertEqual(client.wait_calls, [0, 7, 8])
        self.assertEqual(client.drains, 3)
        self.assertTrue(state.online)
        self.assertEqual(state.stored_total, 1)

    def test_kick_forces_a_holdings_report(self):
        client = FakeShallot([{"seq": 2, "reason": "start", "pending": 0},
                             {"seq": 3, "reason": "kick", "pending": 0},
                             {"seq": 4, "reason": "timeout", "pending": 0}])
        state = shallot_sync.SyncState()
        self.run_loop(client, state, 3)
        # first pass reports (startup), the pass after the kick reports again
        self.assertEqual(self.reports, 2)

    def test_old_shallot_falls_back_to_polling(self):
        client = FakeShallot([None, None])
        state = shallot_sync.SyncState()
        state.wake.set()                 # skip the poll sleep
        orig_wait = state.wake.wait

        def quick_wait(t=None):
            state.wake.set()
            return orig_wait(0)

        state.wake.wait = quick_wait
        self.run_loop(client, state, 2)
        self.assertEqual(client.wait_calls, [0, 0])
        self.assertEqual(client.drains, 2)

    def test_offline_marks_state_and_retries(self):
        client = FakeShallot([urllib.error.URLError("down"), {"seq": 1, "reason": "start", "pending": 0}])
        state = shallot_sync.SyncState()
        orig_wait = state.wake.wait
        state.wake.wait = lambda t=None: orig_wait(0)
        seen = []
        orig_drain = shallot_sync.drain

        def drain(c, s, v=True):
            seen.append(state.online)
            return orig_drain(c, s, v)

        shallot_sync.drain = drain
        self.run_loop(client, state, 2)
        self.assertEqual(seen[1], False)          # after the outage
        self.assertTrue(state.online)              # and back


class CommandTests(LiveLoopTests):
    """Commands from the web UI arrive with a wait answer and are reported back."""

    def test_ping_pause_resume_and_reports(self):
        client = FakeShallot([{"seq": 1, "reason": "start", "pending": 0},
                             {"seq": 2, "reason": "cmd", "pending": 0, "cmd": "ping", "cmd_id": 5},
                             {"seq": 3, "reason": "cmd", "pending": 2, "cmd": "pause", "cmd_id": 6},
                             {"seq": 4, "reason": "new", "pending": 3},
                             {"seq": 5, "reason": "cmd", "pending": 3, "cmd": "resume", "cmd_id": 7},
                             {"seq": 6, "reason": "timeout", "pending": 0}])
        state = shallot_sync.SyncState()
        self.run_loop(client, state, 6)
        self.assertEqual([d[0] for d in client.done], [5, 6, 7])
        self.assertTrue(all(d[1] for d in client.done))
        self.assertIn("pong", client.done[0][2])
        # while paused nothing is collected: 6 waits, 2 passes skipped
        self.assertEqual(client.drains, 4)
        self.assertEqual(client.infos[3]["paused"], "1")
        self.assertEqual(client.infos[5]["paused"], "0")
        self.assertIn("pause", client.infos[0]["caps"].split(","))
        self.assertEqual(client.infos[0]["kind"], "console")
        self.assertFalse(self.store.manifest.get("paused"))

    def test_pause_survives_a_restart(self):
        self.store.manifest["paused"] = True
        client = FakeShallot([{"seq": 1, "reason": "start", "pending": 0}])
        state = shallot_sync.SyncState()
        self.run_loop(client, state, 1)
        self.assertEqual(client.drains, 0)
        self.assertEqual(client.infos[0]["paused"], "1")

    def test_container_refuses_quit_and_update(self):
        client = FakeShallot([{"seq": 1, "reason": "cmd", "pending": 0, "cmd": "quit", "cmd_id": 9},
                             {"seq": 2, "reason": "timeout", "pending": 0}])
        state = shallot_sync.SyncState(kind="docker")
        self.run_loop(client, state, 2)
        self.assertEqual(client.done[0][:2], (9, False))
        self.assertNotIn("quit", client.infos[0]["caps"])
        self.assertEqual(client.infos[0]["kind"], "docker")

    def test_quit_ends_the_loop(self):
        client = FakeShallot([{"seq": 1, "reason": "cmd", "pending": 0, "cmd": "quit", "cmd_id": 3},
                             {"seq": 2, "reason": "timeout", "pending": 0}])
        state = shallot_sync.SyncState()
        self.run_loop(client, state, 5)
        self.assertEqual(len(client.wait_calls), 1)
        self.assertEqual(client.done[0][:2], (3, True))

    def test_relay_settings_from_the_ps5(self):
        made = []
        orig = shallot_sync.Vault

        class FakeVault:
            def __init__(self, store, vcfg):
                made.append(vcfg)
                self.cfg, self.source = vcfg, vcfg.get("source", "own")
                self.state = {"state": "ok", "pending": 0, "at": 1, "error": ""}

            def step(self, budget_s=20.0, force=False):
                return False

        shallot_sync.Vault = FakeVault
        try:
            client = FakeShallot([{"seq": 1, "reason": "start", "pending": 0},
                                 {"seq": 2, "reason": "config", "pending": 0},
                                 {"seq": 3, "reason": "timeout", "pending": 0}])
            client.relay = {"route": "pc", "url": "http://v:8091", "token": "t" * 30, "key": "ab" * 32}
            orig_wait = client.wait

            def wait(seq, timeout=shallot_sync.WAIT_S, info=None):
                if len(client.wait_calls) == 1:
                    client.relay = {"route": "off"}      # owner switched the mode back
                return orig_wait(seq, timeout, info)

            client.wait = wait
            state = shallot_sync.SyncState()
            self.run_loop(client, state, 3)
        finally:
            shallot_sync.Vault = orig
        self.assertEqual(len(made), 1)
        self.assertEqual(made[0]["key_hex"], "ab" * 32)
        self.assertEqual(made[0]["source"], "ps5")
        self.assertIsNone(client.vault_state)          # relay dropped after the "config" wake

    def test_update_installs_and_asks_for_a_restart(self):
        tmp = tempfile.mkdtemp(prefix="shallot-upd-")
        fake = os.path.join(tmp, "shallot_sync.py")
        with open(fake, "w") as f:
            f.write("old")
        orig_file = shallot_sync.__file__
        shallot_sync.__file__ = fake
        shallot_sync.ALLOW_REMOTE_UPDATE = True       # config.json "allow_remote_update": true
        try:
            client = FakeShallot([{"seq": 1, "reason": "cmd", "pending": 0, "cmd": "update", "cmd_id": 4},
                                 {"seq": 2, "reason": "timeout", "pending": 0}])
            client.client_script = lambda: b'CLIENT_VERSION = "9.0"\nprint(1)\n'
            state = shallot_sync.SyncState()
            rc = shallot_sync.run_forever(client, self.store, False, POL, state=state)
            self.assertEqual(rc, shallot_sync.RESTART)
            with open(fake) as f:
                self.assertIn('"9.0"', f.read())
            self.assertTrue(os.path.exists(fake + ".bak"))
            self.assertIn("9.0", client.done[0][2])
            # a script that does not parse is never installed
            client2 = FakeShallot([])
            client2.client_script = lambda: b'CLIENT_VERSION = "9.1"\nthis is not python(\n'
            with self.assertRaises(SyntaxError):
                shallot_sync.update_client(client2)
            with open(fake) as f:
                self.assertIn('"9.0"', f.read())
        finally:
            shallot_sync.__file__ = orig_file
            shallot_sync.ALLOW_REMOTE_UPDATE = False
            shutil.rmtree(tmp, ignore_errors=True)

    def test_update_needs_a_yes_on_the_pc(self):
        # a terminal or container client without the config flag cannot be updated from the web UI
        self.assertNotIn("update", shallot_sync.client_caps("console"))
        self.assertNotIn("update", shallot_sync.client_caps("docker"))
        if os.name == "nt":
            self.assertIn("update", shallot_sync.client_caps("tray"))      # the tray asks first
        client = FakeShallot([{"seq": 1, "reason": "cmd", "pending": 0, "cmd": "update", "cmd_id": 4},
                             {"seq": 2, "reason": "timeout", "pending": 0}])
        state = shallot_sync.SyncState()
        self.run_loop(client, state, 2)
        self.assertEqual(client.done[0][:2], (4, False))
        self.assertFalse(state.restart)

    def test_client_key_header(self):
        c = shallot_sync.ShallotClient(dict(shallot_sync.DEFAULT_CONFIG, ps5_host="h", port=1, client_key="abc123"))
        self.assertIn(("X-Shallot-Key", "abc123"), c.opener.addheaders)
        c2 = shallot_sync.ShallotClient(dict(shallot_sync.DEFAULT_CONFIG, ps5_host="h", port=1))
        self.assertNotIn("X-Shallot-Key", dict(c2.opener.addheaders))


class StandbyAndAckTests(LiveLoopTests):
    def test_standby_does_nothing(self):
        client = FakeShallot([{"seq": 1, "reason": "start", "pending": 0, "standby": True},
                             {"seq": 2, "reason": "timeout", "pending": 0, "standby": True},
                             {"seq": 3, "reason": "config", "pending": 2},
                             {"seq": 4, "reason": "timeout", "pending": 0}])
        state = shallot_sync.SyncState()
        self.run_loop(client, state, 4)
        # the first pass runs before the first answer; afterwards nothing until it is active again
        self.assertEqual(client.drains, 2)
        self.assertFalse(state.standby)
        self.assertEqual(self.reports, 1)

    def test_vault_acks_reported_once(self):
        sent = []

        class C:
            def vault_acks(self, ids):
                sent.append(list(ids))
                return True

        self.store.manifest["vault"] = {"a1": {"at": 1}, "a2": {"at": 1}, "file:abc": {"at": 1}}
        self.assertEqual(shallot_sync.report_vault_acks(C(), self.store), 2)
        self.assertEqual(sorted(sent[0]), ["a1", "a2"])
        self.assertEqual(shallot_sync.report_vault_acks(C(), self.store), 0)
        self.assertEqual(len(sent), 1)

    def test_client_id_stable(self):
        a = self.store.client_id()
        self.assertEqual(len(a), 24)
        self.assertEqual(shallot_sync.Store(self.tmp).client_id(), a)
        c = shallot_sync.ShallotClient(dict(shallot_sync.DEFAULT_CONFIG, ps5_host="h", port=1))
        c.cid = a
        self.assertIn("cid=" + a, c._url("/api/backup/queue", pending="1"))


class ThreadTests(unittest.TestCase):
    """--tray creates the store on the main thread and syncs on another."""

    def test_block_store_usable_from_the_sync_thread(self):
        tmp = tempfile.mkdtemp(prefix="shallot-thread-test-")
        try:
            store = shallot_sync.Store(tmp, dedup={"enabled": True})
            self.assertIsNotNone(store.blocks)
            errors = []

            def use():
                try:
                    store.blocks.has("nothing")
                    store.blocks.stats()
                except Exception as e:      # noqa: BLE001
                    errors.append(e)

            th = threading.Thread(target=use)
            th.start()
            th.join()
            self.assertEqual(errors, [])
            store.blocks.close()
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_unexpected_error_does_not_end_the_loop(self):
        client = FakeShallot([{"seq": 1, "reason": "start", "pending": 0}])
        state = shallot_sync.SyncState()
        calls = []
        orig = shallot_sync.drain

        def drain(c, s, v=True):
            calls.append(1)
            if len(calls) == 1:
                raise RuntimeError("boom")
            state.stop.set()
            return (0, 0)

        saved = (shallot_sync.sync_snapshots, shallot_sync.report_manifest)
        shallot_sync.drain = drain
        shallot_sync.sync_snapshots = lambda c, s: None
        shallot_sync.report_manifest = lambda c, s: 0
        orig_wait = state.wake.wait
        state.wake.wait = lambda t=None: orig_wait(0)
        try:
            shallot_sync.run_forever(client, shallot_sync.Store(tempfile.mkdtemp(prefix="shallot-x-")), False, POL,
                                    state=state)
        finally:
            shallot_sync.drain = orig
            shallot_sync.sync_snapshots, shallot_sync.report_manifest = saved
        self.assertEqual(len(calls), 2)          # the loop carried on after the error


class ServeRequestsTests(unittest.TestCase):
    def test_sends_requested_archive_from_the_block_store(self):
        import hashlib
        import zipfile
        tmp = tempfile.mkdtemp(prefix="shallot-serve-test-")
        try:
            store = shallot_sync.Store(tmp, dedup={"enabled": True})
            z = os.path.join(tmp, "PPSA01234", "sdimg_Save04", "a1.zip")
            os.makedirs(os.path.dirname(z))
            with zipfile.ZipFile(z, "w", zipfile.ZIP_STORED) as zf:
                zf.writestr("PPSA01234/sdimg_Save04", os.urandom(20000))
            sha = shallot_sync.sha256_file(z)
            store.manifest["archives"]["a1"] = {"path": os.path.relpath(z, tmp), "sha256": sha, "size": os.path.getsize(z),
                                                "title_id": "PPSA01234", "save_name": "sdimg_Save04", "created_at": 1, "acked": True}
            store.dedup("a1")
            self.assertTrue(store.in_blocks(store.manifest["archives"]["a1"]))
            sent = {}

            class C:
                def requests(self):
                    return [{"id": "a1", "sha256": sha}, {"id": "unknown", "sha256": "0" * 64}]

                def readopt(self, key, path):
                    with open(path, "rb") as f:
                        sent[key] = hashlib.sha256(f.read()).hexdigest()
                    return {"ok": True}

            self.assertEqual(shallot_sync.serve_requests(C(), store), 1)
            self.assertEqual(sent, {"a1": sha})
            self.assertEqual([n for n in os.listdir(tmp) if n.startswith("shallot_send_")], [])   # temp removed
            store.blocks.close()
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_old_shallot(self):
        class C:
            def requests(self):
                return None
        self.assertIsNone(shallot_sync.serve_requests(C(), None))


class AutostartTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="shallot-auto-test-")
        self.old = os.environ.get("APPDATA")
        os.environ["APPDATA"] = self.tmp
        os.makedirs(os.path.join(self.tmp, "Microsoft", "Windows", "Start Menu", "Programs", "Startup"))

    def tearDown(self):
        if self.old is None:
            os.environ.pop("APPDATA", None)
        else:
            os.environ["APPDATA"] = self.old
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_on_off(self):
        cfg = os.path.join(self.tmp, "my config.json")
        self.assertFalse(shallot_sync.autostart_enabled())
        p = shallot_sync.set_autostart(True, cfg)
        self.assertTrue(shallot_sync.autostart_enabled())
        with open(p, encoding="utf-8") as f:
            vbs = f.read()
        self.assertIn("--tray", vbs)
        self.assertIn('""%s""' % os.path.abspath(cfg), vbs)   # quoted for VBScript
        self.assertIn(", 0, False", vbs)                       # hidden window
        shallot_sync.set_autostart(False, cfg)
        self.assertFalse(shallot_sync.autostart_enabled())


if __name__ == "__main__":
    unittest.main()
