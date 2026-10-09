"""
End-to-end test of the two Docker images (pc/ and vault/) without a PS5:
a stand-in Shallot on this machine queues one archive, the shallot-sync
container collects it, answers web-UI commands and, given the vault settings
by the "PS5" ("PC and VPS", PC route), uploads it to the shallot-vault
container over the signed port. Then this script reads the archive back from
the vault with the same key.

Needs Docker (Desktop) and the 'cryptography' package. Run from the repo root:
    py -3 tests/docker/docker_e2e.py            (KEEP=1 leaves the containers)
"""
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "pc"))
import shallot_sync  # noqa: E402

PS5_PORT, VAULT_PORT = 18082, 18091
TOKEN = "docker-e2e-" + "x" * 24
KEY = os.urandom(32).hex()
QID = "PPSA01234-sdimg_Save04-1791000000-1"


def make_archive():
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as z:
        z.writestr("PPSA01234/sdimg_Save04", os.urandom(300 * 1024))
    return buf.getvalue()


class FakePs5:
    def __init__(self):
        self.zip = make_archive()
        self.sha = hashlib.sha256(self.zip).hexdigest()
        self.acked = None
        self.cmds = []                  # [(id, cmd)] still to hand out
        self.done = {}                  # id -> (ok, result)
        self.infos = []
        self.vault_acked = set()
        self.seq = 1
        self.next_id = 1
        self.lock = threading.Lock()
        self.cv = threading.Condition(self.lock)

    def command(self, cmd):
        with self.cv:
            cid = self.next_id
            self.next_id += 1
            self.cmds.append((cid, cmd))
            self.seq += 1
            self.cv.notify_all()
            return cid

    def handler(self):
        ps5 = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _json(self, obj, code=200):
                b = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)

            def do_GET(self):
                u = urllib.parse.urlsplit(self.path)
                q = dict(urllib.parse.parse_qsl(u.query))
                if u.path == "/api/backup/queue":
                    item = {"id": QID, "sha256": ps5.sha, "size": len(ps5.zip), "title_id": "PPSA01234",
                            "save_name": "sdimg_Save04", "user_id": "10000000", "created_at": 1791000000, "state": "ready"}
                    return self._json({"queue": [] if ps5.acked else [item]})
                if u.path == "/api/backup/queue_download":
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(ps5.zip)))
                    self.send_header("X-Shallot-SHA256", ps5.sha)
                    self.end_headers()
                    self.wfile.write(ps5.zip)
                    return
                if u.path == "/api/backup/wait":
                    ps5.infos.append(q)
                    seq = int(q.get("seq", 0))
                    with ps5.cv:
                        if seq > 0 and seq == ps5.seq:
                            ps5.cv.wait(min(3, int(q.get("timeout", 3))))
                        ans = {"seq": ps5.seq, "reason": "cmd" if ps5.cmds else "timeout", "pending": 0 if ps5.acked else 1}
                        if ps5.cmds:
                            cid, cmd = ps5.cmds.pop(0)
                            ans.update(cmd=cmd, cmd_id=cid)
                    return self._json(ans)
                if u.path == "/api/pc/vault_config":
                    return self._json({"route": "pc", "url": "http://host.docker.internal:%d" % VAULT_PORT,
                                       "token": TOKEN, "key": KEY})
                if u.path == "/api/pc/requests":
                    return self._json({"requests": []})
                if u.path == "/api/backup/snapshots":
                    return self._json({"snapshots": []})
                return self._json({"error": "not found"}, 404)

            def do_POST(self):
                u = urllib.parse.urlsplit(self.path)
                q = dict(urllib.parse.parse_qsl(u.query))
                n = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(n) if n else b""
                if u.path == "/api/backup/queue_ack":
                    ps5.acked = q.get("sha256")
                    return self._json({"ok": True})
                if u.path == "/api/pc/command_done":
                    ps5.done[int(q["id"])] = (q.get("ok") == "1", q.get("result", ""))
                    return self._json({"ok": True})
                if u.path == "/api/pc/vault_acks":
                    ps5.vault_acked.update(json.loads(body or b"{}").get("ids", []))
                    return self._json({"ok": True})
                if u.path == "/api/backup/pc_manifest":
                    return self._json({"ok": True})
                return self._json({"error": "not found"}, 404)

        return H


def docker(*args, check=True, capture=False):
    r = subprocess.run(["docker"] + list(args), capture_output=capture, text=True)
    if check and r.returncode:
        raise SystemExit("docker %s failed:\n%s" % (" ".join(args), (r.stderr or "") if capture else ""))
    return r.stdout if capture else r.returncode


def wait_for(what, cond, timeout=120):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if cond():
            print("  ok:", what)
            return True
        time.sleep(1)
    print("  FAILED:", what)
    return False


def main():
    keep = os.environ.get("KEEP") == "1"
    print("building images")
    docker("build", "-q", "-t", "shallot-sync:test", os.path.join(ROOT, "pc"))
    docker("build", "-q", "-t", "shallot-vault:test", os.path.join(ROOT, "vault"))
    for name in ("shallot-e2e-sync", "shallot-e2e-vault"):
        docker("rm", "-f", name, check=False, capture=True)
    tmp = tempfile.mkdtemp(prefix="shallot-docker-e2e-")
    ps5 = FakePs5()
    srv = ThreadingHTTPServer(("0.0.0.0", PS5_PORT), ps5.handler())
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    ok = True
    try:
        docker("run", "-d", "--name", "shallot-e2e-vault", "-e", "SHALLOT_VAULT_TOKEN=" + TOKEN, "-e", "SHALLOT_VAULT_SIGNED_PORT=8091",
               "-p", "%d:8091" % VAULT_PORT, "shallot-vault:test", capture=True)
        docker("run", "-d", "--name", "shallot-e2e-sync", "--add-host", "host.docker.internal:host-gateway",
               "-e", "SHALLOT_PS5_HOST=host.docker.internal", "-e", "SHALLOT_PS5_PORT=%d" % PS5_PORT,
               "-v", os.path.join(tmp, "backups") + ":/backups", "-v", os.path.join(tmp, "config") + ":/config",
               "shallot-sync:test", capture=True)
        ok &= wait_for("the container collected the archive and acknowledged it with the right SHA-256",
                       lambda: ps5.acked == ps5.sha)
        ok &= wait_for("it reports itself as a Docker client that is not paused",
                       lambda: any(i.get("kind") == "docker" and i.get("paused") == "0" and "quit" not in i.get("caps", "")
                                   for i in ps5.infos))
        c_ping = ps5.command("ping")
        ok &= wait_for("ping answered", lambda: c_ping in ps5.done and ps5.done[c_ping][0] and "docker" in ps5.done[c_ping][1])
        c_pause = ps5.command("pause")
        ok &= wait_for("pause", lambda: c_pause in ps5.done and ps5.infos[-1].get("paused") == "1")
        c_resume = ps5.command("resume")
        ok &= wait_for("resume", lambda: c_resume in ps5.done and ps5.infos[-1].get("paused") == "0")
        c_quit = ps5.command("quit")
        ok &= wait_for("quit refused in a container", lambda: c_quit in ps5.done and not ps5.done[c_quit][0])
        c_vault = ps5.command("vault")
        ok &= wait_for("send to the VPS (relay with the PS5's key)", lambda: c_vault in ps5.done and ps5.done[c_vault][0])
        ok &= wait_for("the PC confirms the vault upload to the PS5", lambda: QID in ps5.vault_acked)
        ok &= wait_for("the vault state reaches the PS5 as 'ok' from the PS5's settings",
                       lambda: any(i.get("vault") == "ok" and i.get("vault_src") == "ps5" for i in ps5.infos))
        # read it back from the vault on this machine, as a rebuilt PC would
        store = shallot_sync.Store(os.path.join(tmp, "readback"), dedup={"enabled": False})
        v = shallot_sync.Vault(store, dict(shallot_sync.DEFAULT_CONFIG["vault"], url="http://127.0.0.1:%d" % VAULT_PORT,
                                          token=TOKEN, key_hex=KEY, source="ps5"))
        dst = os.path.join(tmp, "back.zip")
        v.fetch_archive(QID, dst)
        same = shallot_sync.sha256_file(dst) == ps5.sha
        print("  %s: archive read back from the vault container byte for byte" % ("ok" if same else "FAILED"))
        ok &= same
        files = [f for _, _, fs in os.walk(os.path.join(tmp, "backups")) for f in fs]
        print("  backups folder on the host:", len(files), "file(s)")
        ok &= bool(files)
    finally:
        if not ok or keep:
            print(docker("logs", "--tail", "40", "shallot-e2e-sync", check=False, capture=True))
        if not keep:
            docker("rm", "-f", "shallot-e2e-sync", "shallot-e2e-vault", check=False, capture=True)
        srv.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)
    print("DOCKER E2E", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
