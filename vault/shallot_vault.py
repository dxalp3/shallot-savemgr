#!/usr/bin/env python3
"""
Shallot vault — off-site store for the Shallot SaveMgr PC client (stdlib only).
Shallot (dxalp3) is a fork of Garlic SaveMgr (earthonion): https://git.etawen.dev/earthonion/garlic-savemgr

The PC client encrypts everything before upload; this server only keeps
opaque, content-addressed blobs and never sees a save or a key:

    GET    /v1/ping                         -> {"ok": true, "version": ...}
    POST   /v1/blocks/missing   {"ids": []} -> {"missing": [...]}   (which blocks it lacks)
    POST   /v1/blocks/batch     records    (upload many: [32-byte id][4-byte len][blob] ..., <= 32 MiB)
    POST   /v1/blocks/fetch     {"ids": []} -> records in the same format (download many)
    PUT    /v1/blocks/<id>                  (encrypted block, <= 64 KiB; id = 64 hex chars)
    GET    /v1/blocks/<id>
    POST   /v1/blocks/delete    {"ids": []} (client-driven garbage collection)
    GET    /v1/blocks/                      -> {"ids": [...]}   (for garbage collection)
    PUT    /v1/meta/<kind>/<name>           (encrypted manifest, <= 16 MiB)
    GET    /v1/meta/<kind>/<name>
    DELETE /v1/meta/<kind>/<name>
    GET    /v1/meta/<kind>/                 -> {"names": [...]}
    GET    /v1/stats                        -> counts and bytes
    POST   /v1/gc       {"min_age_s": 86400} -> {"deleted": n}  (blocks no archive uses)

Kinds: archive, snapshot, state, and refs: next to each archive (same name)
its block ids in the clear (32 bytes each; keyed hashes, so they say nothing
about the contents). With them the vault deletes unused blocks itself; it
refuses while any archive has no refs entry, and keeps blocks younger than
min_age_s (an upload in progress writes its blocks before its refs).
Every answer carries X-Shallot-Server-Time, so a client whose clock is off
can correct the time it signs with.

Every request needs "Authorization: Bearer <SHALLOT_VAULT_TOKEN>" (HTTPS
port, behind Caddy) or a signature (any port):
    X-Shallot-Time: <unix seconds>   X-Shallot-Nonce: <16-64 hex>
    X-Shallot-Sig: hex HMAC-SHA256(token, METHOD\nPATH\nTIME\nNONCE\nsha256hex(body))
The PS5 has no TLS, so it signs its requests and talks to the plain port
SHALLOT_VAULT_SIGNED_PORT, which accepts signed requests only: the token
never travels, replays are refused (5 min window + nonce memory), and the
contents are encrypted end to end anyway.

    SHALLOT_VAULT_TOKEN=... SHALLOT_VAULT_DATA=/data python shallot_vault.py [port]
"""
import hashlib
import hmac
import json
import time
import os
import re
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = "2"
BLOCK_MAX = 64 * 1024
META_MAX = 16 * 1024 * 1024
HEX64 = re.compile(r"^[0-9a-f]{64}$")
NAME = re.compile(r"^[0-9a-f]{64}$")
KINDS = ("archive", "snapshot", "state", "refs")


class Vault:
    def __init__(self, root, token):
        self.root = os.path.abspath(root)
        self.token = token.encode("utf-8")
        os.makedirs(os.path.join(self.root, "blocks"), exist_ok=True)
        for k in KINDS:
            os.makedirs(os.path.join(self.root, "meta", k), exist_ok=True)
        self.lock = threading.Lock()
        self.nonces = {}                 # nonce -> expiry, for signed requests
        self.gc_lock = threading.Lock()

    def signed_ok(self, method, path, headers, body):
        ts, nonce, sig = headers.get("X-Shallot-Time"), headers.get("X-Shallot-Nonce"), headers.get("X-Shallot-Sig")
        if not (ts and nonce and sig) or not re.match(r"^[0-9a-f]{16,64}$", nonce) or not ts.isdigit():
            return False
        now = int(time.time())
        if abs(now - int(ts)) > 300:
            return False
        msg = "\n".join((method, path, ts, nonce, hashlib.sha256(body or b"").hexdigest())).encode("utf-8")
        want = hmac.new(self.token, msg, hashlib.sha256).hexdigest()
        if not hmac.compare_digest(want, sig.strip().lower()):
            return False
        with self.lock:
            for n in [n for n, exp in self.nonces.items() if exp < now]:
                del self.nonces[n]
            if nonce in self.nonces:
                return False             # replay
            self.nonces[nonce] = now + 600
        return True

    def block_path(self, bid):
        return os.path.join(self.root, "blocks", bid[:2], bid[2:4], bid)

    def meta_path(self, kind, name):
        return os.path.join(self.root, "meta", kind, name)

    @staticmethod
    def write_atomic(path, data):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".up_")
        try:
            with os.fdopen(fd, "wb") as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, path)
        except BaseException:
            if os.path.exists(tmp):
                os.remove(tmp)
            raise

    def authorized(self, header):
        if not header or not header.startswith("Bearer "):
            return False
        return hmac.compare_digest(header[7:].strip().encode("utf-8"), self.token)

    def stats(self):
        n = size = 0
        for dp, _, files in os.walk(os.path.join(self.root, "blocks")):
            for f in files:
                if not f.startswith("."):
                    n += 1
                    size += os.path.getsize(os.path.join(dp, f))
        metas = {k: len([x for x in os.listdir(os.path.join(self.root, "meta", k)) if not x.startswith(".")]) for k in KINDS}
        return {"blocks": n, "block_bytes": size, "meta": metas}

    def gc(self, min_age_s=86400):
        """Deletes blocks that no archive's refs list and that are older than min_age_s."""
        with self.gc_lock:
            meta = os.path.join(self.root, "meta")
            archives = {x for x in os.listdir(os.path.join(meta, "archive")) if NAME.match(x)}
            refs = {x for x in os.listdir(os.path.join(meta, "refs")) if NAME.match(x)}
            lacking = archives - refs
            if lacking:
                return {"deleted": 0, "skipped": True,
                        "error": "%d archive(s) have no block list yet; nothing deleted" % len(lacking)}
            for orphan in refs - archives:               # an archive deleted without its list
                try:
                    os.remove(os.path.join(meta, "refs", orphan))
                except OSError:
                    pass
            live = set()
            for name in archives:
                with open(os.path.join(meta, "refs", name), "rb") as f:
                    raw = f.read()
                live.update(raw[i:i + 32].hex() for i in range(0, len(raw) - 31, 32))
            cutoff, deleted = time.time() - max(0, int(min_age_s)), 0
            for dp, _, files in os.walk(os.path.join(self.root, "blocks")):
                for f in files:
                    p = os.path.join(dp, f)
                    if HEX64.match(f) and f not in live and os.path.getmtime(p) < cutoff:
                        os.remove(p)
                        deleted += 1
            return {"deleted": deleted, "kept": len(live)}

    def all_blocks(self):
        out = []
        for dp, _, files in os.walk(os.path.join(self.root, "blocks")):
            out += [f for f in files if HEX64.match(f)]
        return out


def make_handler(vault, signed_only=False):
    class H(BaseHTTPRequestHandler):
        server_version = "ShallotVault/" + VERSION

        def log_message(self, fmt, *args):
            sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

        def _send(self, code, obj=None, raw=None, ctype="application/json"):
            data = raw if raw is not None else json.dumps(obj or {}).encode("utf-8")
            self.send_response(code)
            self.send_header("X-Shallot-Server-Time", str(int(time.time())))
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def _read_body(self):
            n = int(self.headers.get("Content-Length") or 0)
            if n < 0 or n > 33 * 1024 * 1024:
                return None
            data = b""
            while len(data) < n:
                chunk = self.rfile.read(n - len(data))
                if not chunk:
                    return None
                data += chunk
            return data

        def _body(self, limit):
            b = self._raw
            return None if b is None or len(b) > limit else b

        def _route(self, method):
            self._raw = self._read_body()
            bearer = not signed_only and vault.authorized(self.headers.get("Authorization"))
            if not bearer and not vault.signed_ok(method, self.path, self.headers, self._raw):
                return self._send(401, {"error": "unauthorized"})
            p = self.path.split("?", 1)[0]
            parts = [x for x in p.split("/") if x]
            if parts[:1] != ["v1"]:
                return self._send(404, {"error": "not found"})
            parts = parts[1:]
            if parts == ["ping"] and method == "GET":
                return self._send(200, {"ok": True, "version": VERSION})
            if parts == ["stats"] and method == "GET":
                return self._send(200, vault.stats())
            if parts == ["gc"] and method == "POST":
                try:
                    opts = json.loads(self._raw or b"{}")
                except ValueError:
                    opts = {}
                return self._send(200, vault.gc(int(opts.get("min_age_s", 86400))))
            if parts[:1] == ["blocks"]:
                return self._blocks(method, parts[1:])
            if parts[:1] == ["meta"] and len(parts) >= 2 and parts[1] in KINDS:
                return self._meta(method, parts[1], parts[2] if len(parts) > 2 else None)
            return self._send(404, {"error": "not found"})

        def _blocks(self, method, rest):
            if method == "POST" and rest == ["batch"]:
                body = self._body(32 * 1024 * 1024)
                if body is None:
                    return self._send(413, {"error": "batch too large or incomplete"})
                pos = stored = 0
                while pos + 36 <= len(body):
                    bid = body[pos:pos + 32].hex()
                    n = int.from_bytes(body[pos + 32:pos + 36], "big")
                    blob = body[pos + 36:pos + 36 + n]
                    if n > BLOCK_MAX or len(blob) != n:
                        return self._send(400, {"error": "bad record"})
                    path = vault.block_path(bid)
                    if not os.path.exists(path):
                        vault.write_atomic(path, blob)
                        stored += 1
                    pos += 36 + n
                if pos != len(body):
                    return self._send(400, {"error": "trailing bytes"})
                return self._send(200, {"stored": stored})
            if method == "POST" and rest == ["fetch"]:
                try:
                    ids = json.loads(self._body(4 * 1024 * 1024) or b"{}").get("ids", [])
                except ValueError:
                    return self._send(400, {"error": "bad json"})
                out = []
                for i in ids[:8192]:
                    if not (isinstance(i, str) and HEX64.match(i)):
                        continue
                    try:
                        with open(vault.block_path(i), "rb") as f:
                            b = f.read()
                    except FileNotFoundError:
                        continue
                    out.append(bytes.fromhex(i) + len(b).to_bytes(4, "big") + b)
                return self._send(200, raw=b"".join(out), ctype="application/octet-stream")
            if method == "POST" and rest in (["missing"], ["delete"]):
                body = self._body(32 * 1024 * 1024)
                try:
                    ids = json.loads(body or b"{}").get("ids", [])
                except ValueError:
                    return self._send(400, {"error": "bad json"})
                ids = [i for i in ids if isinstance(i, str) and HEX64.match(i)]
                if rest == ["missing"]:
                    return self._send(200, {"missing": [i for i in ids if not os.path.exists(vault.block_path(i))]})
                gone = 0
                for i in ids:
                    try:
                        os.remove(vault.block_path(i))
                        gone += 1
                    except FileNotFoundError:
                        pass
                return self._send(200, {"deleted": gone})
            if method == "GET" and rest == []:
                return self._send(200, {"ids": vault.all_blocks()})
            if len(rest) != 1 or not HEX64.match(rest[0]):
                return self._send(400, {"error": "bad block id"})
            path = vault.block_path(rest[0])
            if method == "PUT":
                data = self._body(BLOCK_MAX)
                if data is None:
                    return self._send(413, {"error": "block too large or incomplete"})
                if not os.path.exists(path):
                    vault.write_atomic(path, data)
                return self._send(200, {"ok": True})
            if method == "GET":
                try:
                    with open(path, "rb") as f:
                        return self._send(200, raw=f.read(), ctype="application/octet-stream")
                except FileNotFoundError:
                    return self._send(404, {"error": "no such block"})
            return self._send(405, {"error": "method not allowed"})

        def _meta(self, method, kind, name):
            if name is None:
                if method != "GET":
                    return self._send(405, {"error": "method not allowed"})
                d = os.path.join(vault.root, "meta", kind)
                return self._send(200, {"names": sorted(x for x in os.listdir(d) if NAME.match(x))})
            if not NAME.match(name):
                return self._send(400, {"error": "bad name"})
            path = vault.meta_path(kind, name)
            if method == "PUT":
                data = self._body(META_MAX)
                if data is None:
                    return self._send(413, {"error": "too large or incomplete"})
                vault.write_atomic(path, data)
                return self._send(200, {"ok": True})
            if method == "GET":
                try:
                    with open(path, "rb") as f:
                        return self._send(200, raw=f.read(), ctype="application/octet-stream")
                except FileNotFoundError:
                    return self._send(404, {"error": "not found"})
            if method == "DELETE":
                try:
                    os.remove(path)
                except FileNotFoundError:
                    pass
                return self._send(200, {"ok": True})
            return self._send(405, {"error": "method not allowed"})

        def do_GET(self):
            self._route("GET")

        def do_PUT(self):
            self._route("PUT")

        def do_POST(self):
            self._route("POST")

        def do_DELETE(self):
            self._route("DELETE")

    return H


def serve(root, token, host="0.0.0.0", port=8090, signed_only=False, vault=None):
    if len(token) < 24:
        raise SystemExit("SHALLOT_VAULT_TOKEN must be set (at least 24 characters)")
    httpd = ThreadingHTTPServer((host, port), make_handler(vault or Vault(root, token), signed_only))
    httpd.vault = vault
    return httpd


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else int(os.environ.get("SHALLOT_VAULT_PORT", "8090"))
    root, token = os.environ.get("SHALLOT_VAULT_DATA", "/data"), os.environ.get("SHALLOT_VAULT_TOKEN", "")
    if len(token) < 24:
        raise SystemExit("SHALLOT_VAULT_TOKEN must be set (at least 24 characters)")
    shared = Vault(root, token)
    srv = serve(root, token, port=port, vault=shared)
    sport = int(os.environ.get("SHALLOT_VAULT_SIGNED_PORT", "0") or 0)
    if sport:                                       # plain HTTP, signed requests only (for the PS5)
        ssrv = serve(root, token, port=sport, signed_only=True, vault=shared)
        threading.Thread(target=ssrv.serve_forever, daemon=True).start()
        print("Shallot vault: signed-only port %d" % sport, flush=True)
    print("Shallot vault on port %d, data in %s" % (port, root), flush=True)
    srv.serve_forever()
