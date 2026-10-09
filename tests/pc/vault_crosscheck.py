"""
Cross-check for the VPS only mode: archives the PS5 code uploaded to a vault
(tests/host/vault_e2e.sh, KEEP_VAULT=1) must be readable by the PC client
with the same key. Usage:
    py -3 tests/pc/vault_crosscheck.py <ps5_upload.json> <bearer url> <token>
"""
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "pc"))
import shallot_sync  # noqa: E402


def main():
    data = json.load(open(sys.argv[1], encoding="utf-8"))
    tmp = tempfile.mkdtemp(prefix="shallot-cross-")
    try:
        key_file = os.path.join(tmp, "vault.key")
        with open(key_file, "w", encoding="utf-8") as f:
            f.write(data["key"] + "\n")
        store = shallot_sync.Store(os.path.join(tmp, "store"), dedup={"enabled": True})
        vcfg = dict(shallot_sync.DEFAULT_CONFIG["vault"], url=sys.argv[2], token=sys.argv[3], key_file=key_file)
        v = shallot_sync.Vault(store, vcfg)
        got = v.pull_all()
        ok = True
        for key, sha in data["archives"].items():
            e = store.manifest["archives"].get(key)
            have = shallot_sync.sha256_file(os.path.join(store.root, e["path"])) if e else None
            print("%-60s %s" % (key, "OK" if have == sha else "MISMATCH/MISSING"))
            ok &= have == sha
        snaps = [n for n in os.listdir(os.path.join(store.root, "PPSA00002", "_snapshots"))] \
            if os.path.isdir(os.path.join(store.root, "PPSA00002", "_snapshots")) else []
        print("pulled %d archive(s), %d restore point(s) written by the PS5 code" % (got, len(snaps)))
        for name in snaps:
            sn = json.load(open(os.path.join(store.root, "PPSA00002", "_snapshots", name), encoding="utf-8"))
            ok &= sn.get("format") == "shallot-snapshot"
        if store.blocks is not None:
            store.blocks.close()
        print("CROSS-CHECK", "PASSED" if ok and got == len(data["archives"]) and snaps else "FAILED")
        return 0 if ok else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
