#!/usr/bin/env python3
"""
UI preview server for the Backup & Sync tab (no PS5 needed).

Serves src/ui.html the way the Makefile embeds it (version + i18n injected)
and answers the LAN sync endpoints with realistic mock data modelled on the
owner's console (TLOU2, Atomfall, Pirate Yakuza, Digimon). Upstream endpoints
get minimal stubs so the page loads.

    python tests/ui/mock_server.py [port]      (default 8099)
"""
import glob
import json
import os
import sys
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
NOW = int(time.time())
STATE = {"job": {"active": False, "status": "idle"}, "job_started": 0, "settings": None, "last_quick": None,
         "scan_done_at": 0}

TITLES = [
    ("PPSA15508", "The Last of Us Part II Remastered", 12, True, 0, "HEALTHY", "No action needed"),
    ("PPSA15493", "Atomfall", 10, True, 1, "CHANGED_SINCE_LAST_SCAN",
     "Backup recommended: metadata changed since the last protected version (Back Up Changed Saves checks content)"),
    ("PPSA24701", "Digimon Story Time Stranger", 16, True, 11, "", ""),
    ("PPSA09090", "Like a Dragon: Pirate Yakuza in Hawaii", 2, False, 0, "UNTRACKED",
     "Backup recommended: Shallot has never protected this title"),
]


# Atomfall-like game: five objects, all but Save08 with a PS5 backup copy.
OBJ_IDS = ["Save00", "Save04", "Save05", "Save06", "Save08"]


def objects(tid):
    out = []
    for i, oid in enumerate(OBJ_IDS):
        files = {"primary": {"save_name": "sdimg_" + oid, "state": "changed" if oid == "Save04" else "protected",
                             "current_gen": 20 + i, "last_packaged_at": NOW - 900, "size": 73334942,
                             "pending_pc": oid == "Save06"}}
        if oid != "Save08":
            files["sce_backup"] = {"save_name": "sdimg_sce_bu_" + oid, "state": "protected", "current_gen": 40 + i,
                                   "last_packaged_at": NOW - 900, "size": 73334956, "pending_pc": False}
        out.append({"id": oid, "label": STATE.setdefault("labels", {"Save05": "Main save"}).get(oid, ""),
                    "user_id": "12229390", "is_ps4": False, "orphan": False, "fingerprint": "ab" * 32,
                    "files": files, "state": files["primary"]["state"], "pending_pc": oid == "Save06", "pending_vps": oid in ("Save04", "Save06"),
                    "modified_at": NOW - 600 - i * 5000, "last_backup": NOW - 900})
    out.append({"id": "Save09", "user_id": "12229390", "is_ps4": False, "orphan": True, "fingerprint": "cd" * 32,
                "files": {"sce_backup": {"save_name": "sdimg_sce_bu_Save09", "state": "protected", "current_gen": 60,
                                         "last_packaged_at": NOW - 900, "size": 1234, "pending_pc": False}},
                "state": "protected", "pending_pc": False, "label": "", "modified_at": NOW - 99000, "last_backup": NOW - 900})
    return {"title_id": tid, "name": "Atomfall", "objects": out}


def saves_list():
    """Upstream /api/saves: one row per save file."""
    rows = []
    for tid, name, n, *_ in TITLES:
        names = (["sdimg_" + o for o in OBJ_IDS] + ["sdimg_sce_bu_" + o for o in OBJ_IDS if o != "Save08"]
                 if tid == "PPSA15493" else ["sdimg_SAVE%02d" % i for i in range(min(n, 4))])
        for sn in names:
            rows.append({"title_id": tid, "title_name": name, "save_name": sn, "type": "ps5", "usb": False,
                         "uid": "12229390", "backup": sn.startswith("sdimg_sce_bu_")})
    return {"saves": rows}


def snapshots(tid):
    snaps = []
    fetched = STATE.get("fetch_at") and time.time() >= STATE["fetch_at"]
    for k, (sid, age, reason, bump, ps5) in enumerate([(31, 900, "game_close", 0, True), (27, 7200, "game_close", 1, True),
                                                         (12, 86400 * 9, "migrated", 2, False)]):
        ps5 = ps5 or bool(fetched)
        objs = []
        for i, oid in enumerate(OBJ_IDS):
            g = 20 + i - (bump if oid in ("Save04", "Save06") else 0)
            members = [{"role": "primary", "file": "sdimg_" + oid, "is_ps4": False, "gen_id": g, "sha256": "%064x" % (g * 7919),
                        "size": 73334942, "archive_id": "q%d" % g, "archive_on_ps5": ps5, "on_pc": k > 0}]
            if oid != "Save08":
                members.append({"role": "sce_backup", "file": "sdimg_sce_bu_" + oid, "is_ps4": False, "gen_id": g + 20,
                                "sha256": "%064x" % ((g + 20) * 7919), "size": 73334956, "archive_id": "q%d" % (g + 20),
                                "archive_on_ps5": ps5, "on_pc": k > 0})
            objs.append({"id": oid, "members": members})
        if k == 2:
            objs.append({"id": "Save03", "members": [{"role": "primary", "file": "sdimg_Save03", "is_ps4": False, "gen_id": 3,
                         "sha256": "%064x" % 3, "size": 1, "archive_id": "q3", "archive_on_ps5": False, "on_pc": True}]})
        snaps.append({"format": "shallot-snapshot", "version": 1, "snapshot_id": sid, "kind": "title", "object_id": "",
                      "game": {"title_id": tid, "name": "Atomfall"}, "user_id": "12229390", "created_at": NOW - age,
                      "reason": reason, "object_count": len(objs), "file_count": sum(len(o["members"]) for o in objs),
                      "restorable_on_ps5": ps5, "all_on_pc": k > 0, "objects": objs})
    return {"snapshots": snaps}


OFFERS = [
    {"id": 3, "user_id": "12229390", "title_id": "PPSA15493", "name": "Atomfall", "object_id": "Save04", "detected_at": NOW - 300,
     "reason": "startup_reconcile", "target_snap": 31, "target_created_at": NOW - 900,
     "files": "sdimg_Save04 gen 20 -> 21; sdimg_sce_bu_Save04 gen 40 -> 41", "certain": True, "target_on_ps5": True,
     "target_on_pc": True, "state": "open", "stale": False, "resolved_at": 0,
     "detail": "current version is a known ancestor of the newer one"},
    {"id": 2, "user_id": "12229390", "title_id": "PPSA24701", "name": "Digimon Story Time Stranger", "object_id": "0000.bin",
     "detected_at": NOW - 4000, "reason": "game_close", "target_snap": 12, "target_created_at": NOW - 90000,
     "files": "sdimg_0000.bin gen 7 -> 9", "certain": False, "target_on_ps5": False, "target_on_pc": True, "state": "open",
     "stale": True, "resolved_at": 0, "detail": "the newer version is only on the PC"},
    {"id": 1, "user_id": "12229390", "title_id": "PPSA15493", "name": "Atomfall", "object_id": "Save05", "detected_at": NOW - 86400,
     "reason": "game_close", "target_snap": 9, "target_created_at": NOW - 90000, "files": "sdimg_Save05 gen 3 -> 4",
     "certain": True, "target_on_ps5": True, "target_on_pc": True, "state": "kept", "stale": False, "resolved_at": NOW - 80000,
     "detail": "kept the current version"},
]


def html():
    with open(os.path.join(ROOT, "src", "ui.html"), encoding="utf-8") as f:
        s = f.read().replace("__VERSION__", "mock-preview")
    dicts = ""
    for p in sorted(glob.glob(os.path.join(ROOT, "src", "i18n", "*.js"))):
        with open(p, encoding="utf-8") as f:
            dicts += f.read() + "\n"
    return s.replace('"__I18N_DICTIONARIES__";', dicts)


def default_settings():
    return {"enabled": True, "max_queue_mb": 2048, "keep_acked_hours": 168, "game_close_backup": True,
            "stability_check": True, "stability_interval_s": 2, "stability_samples": 2, "stability_timeout_s": 45,
            "followup_rescan_s": 180, "idle_sweep": True, "idle_delay_min": 10, "startup_reconcile": True,
            "startup_protect_untracked": True, "bidirectional": "ask", "regression_recovery": "ask",
            "rollback_before_restore": True, "storage_mode": "pc", "vps_route": "ps5", "vault_url": "", "vault_token": ""}


def cfg_now():
    return STATE["settings"] or default_settings()


def scan_time():
    """Simulated manual scan: finishes 4 s after it was queued (as if behind a backup job)."""
    d = STATE["scan_done_at"]
    return int(d) if d and time.time() >= d else NOW - 300


def job_state():
    j = dict(STATE["job"])
    if j.get("active"):
        elapsed = time.time() - STATE["job_started"]
        total = j["total"]
        done = min(total, int(elapsed / 1.5))
        j["done"] = done
        j["current"] = TITLES[min(done, total - 1)][0]
        if done >= total:
            j.update(active=False, status="completed", finished_at=int(time.time()), current="",
                     summary="4/4 titles: 1 healthy, 1 changed, 0 missing, 0 regressions, 0 not scanned, 0 errors")
            STATE["job"] = j
            STATE["last_quick"] = int(time.time())
    return j


def pc_state():
    mode = STATE.get("pc_mode", "ok")
    if mode == "never":
        return {"connected": False, "live": False, "name": "", "version": "", "addr": "", "last_seen": 0,
                "backlog": 3, "backlog_bytes": 160_000_000}
    if mode == "off":
        return {"connected": False, "live": False, "name": "DESKTOP-SHALLOT", "version": "2.0", "addr": "192.168.137.1",
                "last_seen": NOW - 7200, "backlog": 5, "backlog_bytes": 290_000_000}
    kind = STATE.get("pc_kind", "tray")
    caps = "ping,sync,pause,resume,thin,vault" + ("" if kind == "docker" else ",update,quit")
    relay = cfg_now().get("storage_mode") == "both" and cfg_now().get("vps_route") == "pc"
    out = {"connected": True, "live": True, "name": "NAS" if kind == "docker" else "DESKTOP-SHALLOT", "version": "2.0",
           "bundled_version": "2.1", "addr": "192.168.137.1", "kind": kind, "caps": caps, "paused": STATE.get("pc_paused", False),
           "held": 412, "held_bytes": 3_100_000_000, "free_bytes": 812_000_000_000,
           "last_seen": int(time.time()), "backlog": 2 if mode == "busy" else 0, "backlog_bytes": 146_000_000,
           "vault": {"state": ("pending" if mode == "busy" else "ok") if relay else "off", "pending": 3,
                     "at": int(time.time()) - 120, "src": "ps5" if relay else ""}}
    if STATE.get("cmd"):
        out["cmd"] = STATE["cmd"]
    return out


def pc_command(what):
    """Simulated client: picks the command up after 0.5 s, answers after 1.5 s."""
    cid = STATE.get("cmd_next", 1)
    STATE["cmd_next"] = cid + 1
    STATE["cmd"] = {"id": cid, "do": what, "state": "waiting", "at": int(time.time()), "done_at": 0, "result": ""}
    STATE["cmd_t"] = time.time()
    return cid


def pc_command_tick():
    c = STATE.get("cmd")
    if not c or c["state"] in ("done", "failed"):
        return
    age = time.time() - STATE["cmd_t"]
    if age > 1.5:
        res = {"ping": "pong from DESKTOP-SHALLOT (tray, v2.0)", "pause": "paused", "resume": "collecting again",
               "thin": "3 old version(s) and 1 restore point(s) thinned out (210.4 MB)", "vault": "VPS copy: ok",
               "sync": "collecting now", "update": "updated to v2.1, restarting", "quit": "quitting"}.get(c["do"], "ok")
        if c["do"] in ("pause", "resume"):
            STATE["pc_paused"] = c["do"] == "pause"
        c.update(state="done", done_at=int(time.time()), result=res)
    elif age > 0.5:
        c["state"] = "running"


def route(method, path, q, body):
    if path == "/api/users":
        return {"users": [{"id": "12229390", "name": "Alperen"}]}
    if path == "/mock/pc":
        STATE["pc_mode"] = q.get("mode", "ok")
        STATE["pc_kind"] = q.get("kind", STATE.get("pc_kind", "tray"))
        return {"ok": True, "mode": STATE["pc_mode"]}
    if path == "/api/pc/command":
        return {"ok": True, "id": pc_command(q.get("do", "")), "pc_connected": True}
    if path == "/api/pc/status":
        pc_command_tick()
        return pc_state()
    if path.startswith("/api/auth/"):
        st = STATE.setdefault("auth", {"pw": ""})
        if path == "/api/auth/status":
            return {"enabled": bool(st["pw"]), "logged_in": bool(st["pw"]), "local": False, "client_key_set": bool(st["pw"])}
        if path == "/api/auth/password":
            b = json.loads(body or "{}")
            if st["pw"] and b.get("current") != st["pw"]:
                return {"error": "the current password is wrong"}
            if b.get("new") and len(b["new"]) < 6:
                return {"error": "use at least 6 characters"}
            st["pw"] = b.get("new", "")
            return {"ok": True, "enabled": bool(st["pw"])}
        if path == "/api/auth/client_key":
            return {"key": "3f9a" * 12}
        return {"ok": True}
    if path == "/api/pc/clients":
        cl = STATE.setdefault("clients", [
            {"id": "a1b2c3", "name": "DESKTOP-SHALLOT", "kind": "tray", "version": "2.2", "addr": "192.168.137.1",
             "last_seen": NOW, "connected": True, "state": "active"},
            {"id": "d4e5f6", "name": "NAS", "kind": "docker", "version": "2.2", "addr": "192.168.137.1",
             "last_seen": NOW - 30, "connected": True, "state": "standin"}])
        if method == "POST":
            cid, act = q.get("id"), q.get("do")
            for c in cl:
                if act == "activate":
                    c["state"] = "active" if c["id"] == cid else ("blocked" if c["state"] == "blocked" else "standby")
                elif c["id"] == cid:
                    c["state"] = {"block": "blocked", "unblock": "standby"}.get(act, c["state"])
            if act == "forget":
                STATE["clients"] = [c for c in cl if c["id"] != cid]
            return {"ok": True}
        return {"active": next((c["id"] for c in cl if c["state"] == "active"), ""), "clients": cl}
    if path == "/api/pc/fetch":
        STATE["fetch_at"] = time.time() + 3
        return {"ok": True, "id": 1, "state": "waiting", "pc_connected": True}
    if path == "/api/pc/fetch_status":
        done = time.time() >= STATE.get("fetch_at", 0)
        return {"id": 1, "state": "ready" if done else "receiving", "got": 2 if done else 1, "n": 2, "error": ""}
    if path.startswith("/api/vault/"):
        if path == "/api/vault/key":
            return {"fingerprint": "3f2a9c1d77e0b412", **({"key": "ab" * 32} if q.get("reveal") else {})}
        return {"ok": True, "queued": True}
    if path == "/api/system/autostart":
        a = q.get("action", "")
        st = STATE.setdefault("auto", {"stored": False, "onionhen": True, "in_payloads": False, "autostart": False})
        if a == "upload":
            st["stored"] = True
        elif a == "on":
            st.update(autostart=True, in_payloads=True)
        elif a == "off":
            st["autostart"] = False
        return st
    if path == "/api/system/launcher":
        if method == "POST":
            STATE["launcher"] = q.get("action") == "install"
            return {"ok": True, "installed": STATE["launcher"]}
        return {"installed": STATE.get("launcher", False), "title_id": "SHAL00001", "last_rc": 0, "last": ""}
    if path == "/api/backup/housekeeping":
        return {"ok": True, "queued": True}
    if path == "/api/backup/kick":
        return {"ok": True, "pc": "notified" if STATE.get("pc_mode", "ok") in ("ok", "busy") else "not connected"}
    if path == "/api/backup/status":
        cfg = cfg_now()
        pc_command_tick()
        direct = cfg["storage_mode"] == "vps" or (cfg["storage_mode"] == "both" and cfg["vps_route"] != "pc")
        return {"build": "lansync Oct  7 2026 12:00:00 (mock)", "storage_mode": cfg["storage_mode"],
                "vps_route": cfg["vps_route"], "ready": True, "enabled": cfg["enabled"],
                "queue": {"count": 31, "bytes": 412_000_000, "pending_pc": 14, "pending_pc_bytes": 96_000_000,
                          "pending_vps": 5, "pending_vps_bytes": 40_000_000, "made": 420, "at_pc": 406, "at_vps": 415,
                          "pending_cloud": 0, "missing": 0, "constrained": False, "max_queue_mb": 2048,
                          "keep_acked_hours": 168},
                "last_backup_queued": NOW - 600, "last_pc_ack": NOW - 3600, "last_cloud_ack": 0,
                "last_startup_recovery": NOW - 1800, "last_startup_reconciliation": NOW - 1780,
                "last_idle_scan": scan_time(), "last_backup": NOW - 600, "pc": pc_state(),
                "store": {"mode": (STATE["settings"] or default_settings()).get("storage_mode", "pc"), "zip_archives": 9,
                          "zip_bytes": 412_000_000, "packed_archives": 37, "packed_logical_bytes": 2_600_000_000,
                          "pack_bytes": 160_000_000, "pack_dead_bytes": 4_000_000, "last_housekeeping": NOW - 3000},
                "vault": {"enabled": direct, "route": "ps5" if direct else ("pc" if cfg["storage_mode"] == "both" else "off"),
                          "configured": bool(cfg.get("vault_url")), "state": "pending" if direct else "off",
                          "pending": 2, "last_ok": NOW - 90, "error": "", "key": "3f2a9c1d77e0b412"},
                "recovery_open": sum(1 for o in STATE.setdefault("offers", OFFERS) if o["state"] == "open"), "rest_resumes": 1,
                "worker": {"running": True, "pending": 0, "last_error": "",
                           "current": "title_closed PPSA24701" if 0 < STATE["scan_done_at"] - time.time() else ""},
                "config": cfg}
    if path == "/api/reconcile/status":
        return {"job": job_state()}
    if path == "/api/backup/titles":
        rows = []
        for tid, name, saves, tracked, pend, res, act in TITLES:
            rows.append({"title_id": tid, "name": name, "saves": saves, "objects": (saves + 1) // 2, "tracked": tracked, "running": False,
                         "recovery_open": sum(1 for o in STATE.setdefault("offers", OFFERS) if o["state"] == "open" and o["title_id"] == tid),
                         "last_backup": NOW - 900 if tracked else 0, "pending_pc": pend, "pending_vps": (pend + 1) // 2 if tracked else 0,
                         "last_result": res,
                         "recommended_action": act, "last_check": NOW - 1200 if res else 0})
        return {"running": "", "titles": rows}
    if path == "/api/reconcile/catalogue/result":
        if "kind=full" in q:
            return {"error": "No catalogue result yet"}
        if not STATE["last_quick"]:
            return {"error": "No catalogue result yet"}
        return {"kind": "catalogue_quick", "workload": "quick", "protect": False, "finished_at": STATE["last_quick"],
                "cancelled": False, "titles": 4, "checked": 4, "skipped_running": 0, "changed_saves": 1,
                "unprotected_changes": 0, "known_regressions": 0, "missing_saves": 0, "not_scanned": 0,
                "conflicts": 0, "errors": 0, "protect_jobs_queued": 0,
                "by_result": {"HEALTHY": 2, "UNTRACKED": 1, "CHANGED_SINCE_LAST_SCAN": 1},
                "anomalous_titles": [{"title_id": "PPSA15493", "name": "Atomfall", "result": "CHANGED_SINCE_LAST_SCAN",
                                      "detail": "PC download pending"}]}
    if path.startswith("/api/reconcile/catalogue/") and method == "POST":
        if STATE["job"].get("active"):
            return {"error": "Another reconciliation job is running"}
        STATE["job"] = {"active": True, "kind": "catalogue_full" if "full" in path else "catalogue_quick",
                        "title_id": "", "protect": "protect=1" in q, "done": 0, "total": 4, "current": "",
                        "started_at": int(time.time()), "finished_at": 0, "status": "running", "summary": ""}
        STATE["job_started"] = time.time()
        return {"ok": True, "started": True}
    if path == "/api/reconcile/cancel":
        STATE["job"].update(active=False, status="cancelled", summary="cancelled")
        return {"ok": True}
    if path == "/api/reconcile/title/sanity":
        return {"title_id": q.get("id"), "check": "sanity", "workload": "quick", "checked_at": int(time.time()),
                "running": False, "tracked": True, "result": "HEALTHY", "recommended_action": "No action needed",
                "pending_pc": 3, "counts": {"HEALTHY": 2},
                "saves": [{"user_id": "12229390", "save_name": "sdimg_SAVEFILE00", "status": "HEALTHY", "pending_pc": False, "detail": "protected"},
                          {"user_id": "12229390", "save_name": "sdimg_sce_bu_SAVEFILE0P", "status": "HEALTHY", "pending_pc": True,
                           "detail": "protected on PS5, PC download pending"}]}
    if path == "/api/reconcile/result":
        tid = q.get("title", "")
        if tid == "PPSA24701":
            return {"error": "No result stored for this title"}
        return {"title_id": tid, "check": "full", "workload": "moderate", "checked_at": NOW - 1200, "running": False,
                "cancelled": False, "tracked": True, "result": "KNOWN_REGRESSION" if tid == "PPSA15493" else "HEALTHY",
                "recommended_action": "Recovery available: the PC holds a newer generation of this save" if tid == "PPSA15493" else "No action needed",
                "pending_pc": 1, "pc_compared": False, "archives": {"checked": 16, "corrupt": 0, "missing": 0},
                "repairs": 0, "repair_log": "",
                "saves": [{"user_id": "12229390", "save_name": "sdimg_Save00", "status": "HEALTHY", "pending_pc": False, "detail": "content = gen 3, on PC"},
                          {"user_id": "12229390", "save_name": "sdimg_Save04", "status": "KNOWN_REGRESSION" if tid == "PPSA15493" else "HEALTHY",
                           "pending_pc": False, "detail": "content = gen 5; PC holds newer gen 9"},
                          {"user_id": "12229390", "save_name": "sdimg_Save07", "status": "HEALTHY", "pending_pc": True, "detail": "content = gen 12, PC download pending"}]}
    if path == "/api/backup/history":
        return {"title_id": q.get("title"), "generations": [
            {"gen_id": 13, "user_id": "12229390", "object_id": "Save04", "role": "sce_backup", "save_name": "sdimg_sce_bu_Save04",
             "sha256": "aa9a761ed723572a", "size": 73334956, "parent_gen": 0, "created_at": NOW - 7000, "reason": "close_followup",
             "queue_id": "w", "pc_acked_at": NOW - 6000, "cloud_acked_at": 0, "archive_on_ps5": True, "is_current": False},
            {"gen_id": 12, "user_id": "12229390", "object_id": "Save07", "role": "primary", "save_name": "sdimg_Save07", "sha256": "e078bbe5661c4b60", "size": 73334942,
             "parent_gen": 0, "created_at": NOW - 900, "reason": "game_close", "queue_id": "x", "pc_acked_at": 0,
             "cloud_acked_at": 0, "archive_on_ps5": True, "is_current": True},
            {"gen_id": 9, "user_id": "12229390", "object_id": "Save04", "role": "primary", "save_name": "sdimg_Save04", "sha256": "2b9a761ed723572a", "size": 73334942,
             "parent_gen": 5, "created_at": NOW - 7200, "reason": "game_close", "queue_id": "y", "pc_acked_at": NOW - 7000,
             "cloud_acked_at": 0, "archive_on_ps5": True, "is_current": False},
            {"gen_id": 5, "user_id": "12229390", "save_name": "sdimg_Save04", "sha256": "ebc1137d66d126f1", "size": 73334942,
             "parent_gen": 0, "created_at": NOW - 86400, "reason": "migrated", "queue_id": "z", "pc_acked_at": NOW - 80000,
             "cloud_acked_at": 0, "archive_on_ps5": False, "is_current": True}]}
    if path == "/api/restore/from_queue":
        STATE["restore_until"] = time.time() + 5
        STATE["restore_kind"] = ("file", "")
        return {"ok": True, "queued": True}
    if path == "/api/restore/snapshot":
        STATE["restore_until"] = time.time() + 5
        STATE["restore_kind"] = ("object" if q.get("object") else "title", q.get("object", ""), q.get("id"))
        return {"ok": True, "queued": True}
    if path == "/api/restore/snapshot_preview":
        return {"snapshot_id": int(q.get("id") or 0), "kind": "title", "title_id": "PPSA15493", "user_id": "12229390",
                "restore_objects": OBJ_IDS,
                "remove": [{"object_id": "Save09", "files": ["sdimg_sce_bu_Save09"]},
                           {"object_id": "Save10", "files": ["sdimg_Save10", "sdimg_sce_bu_Save10"]}] if q.get("id") == "27" else [],
                "remove_files": 3 if q.get("id") == "27" else 0, "remove_fp": "f" * 64}
    if path == "/api/system/probe":
        return {"started": True, "state": 3, "shutdown_in_progress": 0, "standby_enabled": 1, "resumes": 1,
                "last_resume_at": NOW - 7200, "last_suspend_s": 3540, "shutdown_since": 0,
                "suspend_notification_rc": -2137653247, "event_rc": -2137653245, "events_received": 0,
                "previous_session": "console restarted since the previous run (last alive 12 s before this boot)",
                "history": [{"ts": NOW - 7200, "text": "resumed after ~3540 s frozen (rest mode)"},
                            {"ts": NOW - 10800, "text": "first sample: state 3 shutdown 0 standby 1"}]}
    if path == "/api/recovery":
        offers = STATE.setdefault("offers", OFFERS)
        return {"offers": [o for o in offers if not q.get("title") or o["title_id"] == q["title"]]}
    if path == "/api/recovery/resolve":
        for o in STATE.setdefault("offers", OFFERS):
            if str(o["id"]) == q.get("id") and o["state"] == "open":
                o["state"] = "kept" if q.get("action") == "keep" else "restored"
                o["resolved_at"] = int(time.time())
                if q.get("action") == "restore":
                    STATE["restore_until"] = time.time() + 4
                    STATE["restore_kind"] = ("object", o["object_id"])
                return {"ok": True, "queued": True}
        return {"error": "offer is not open"}
    if path == "/api/backup/objects":
        return objects(q.get("title"))
    if path == "/api/backup/snapshots":
        return snapshots(q.get("title"))
    if path == "/api/backup/delete_object":
        STATE["restore_until"] = time.time() + 4
        STATE["restore_kind"] = ("delete", q.get("object", ""))
        return {"ok": True, "queued": True}
    if path == "/api/backup/protect_object":
        return {"ok": True, "queued": True}
    if path == "/api/restore/status":
        u = STATE.get("restore_until", 0)
        if not u:
            return {"restore": {"active": False, "state": "idle"}}
        steps = ["staging", "validating", "rollback", "restoring", "verifying"]
        left = u - time.time()
        if left > 0:
            return {"restore": {"active": True, "state": "running", "step": steps[min(4, int((5 - left)))],
                                "title_id": "PPSA15493", "save_name": "sdimg_Save04"}}
        kind = STATE.get("restore_kind", ("file", ""))
        if kind[0] == "title":
            return {"restore": {"active": False, "state": "failed", "kind": "title", "snap_id": int(kind[2] or 0),
                                "title_id": "PPSA15493", "save_name": "sdimg_Save00", "finished_at": int(u),
                                "message": "sdimg_Save06: could not move the restored image into place; every change was undone",
                                "items": [{"save_name": "sdimg_Save00", "object_id": "Save00", "role": "primary", "result": "unchanged", "note": ""},
                                          {"save_name": "sdimg_Save04", "object_id": "Save04", "role": "primary", "result": "undone", "note": "previous version put back"},
                                          {"save_name": "sdimg_sce_bu_Save04", "object_id": "Save04", "role": "sce_backup", "result": "undone", "note": "previous version put back"},
                                          {"save_name": "sdimg_Save06", "object_id": "Save06", "role": "primary", "result": "failed", "note": "could not move the restored image into place"}]}}
        if kind[0] == "delete":
            return {"restore": {"active": False, "state": "done", "kind": "delete", "object_id": kind[1], "title_id": "PPSA15493",
                                "finished_at": int(u), "message": "deleted save %s (2 file(s)); it is kept in game snapshot #31" % kind[1]}}
        if kind[0] == "object":
            return {"restore": {"active": False, "state": "done", "kind": "object", "object_id": kind[1], "title_id": "PPSA15493",
                                "save_name": "sdimg_" + kind[1], "finished_at": int(u),
                                "message": "restored save %s (2 file(s)); the previous state is game snapshot #32" % kind[1]}}
        return {"restore": {"active": False, "state": "done", "title_id": "PPSA15493", "save_name": "sdimg_Save04",
                            "finished_at": int(u), "message": "restored gen 9; previous version kept as gen 13"}}
    if path == "/api/backup/compare":
        return {"title_id": q.get("title"), "check": "compare_pc", "workload": "quick", "pc_reported_at": NOW - 120,
                "result": "CONFLICT", "recommended_action": "Conflict: PS5 and PC changed independently - choose Keep PS5 / Restore PC / Preserve both",
                "counts": {"SYNCED": 1, "PS5_NEWER": 1, "CONFLICT": 1},
                "saves": [{"user_id": "12229390", "save_name": "sdimg_Save00", "sync_state": "SYNCED", "current_gen": 3,
                           "last_common_gen": 3, "pc_versions": 2, "metadata_changed": False, "detail": "both at gen 3", "action": "No action needed"},
                          {"user_id": "12229390", "save_name": "sdimg_Save04", "sync_state": "CONFLICT", "current_gen": 9,
                           "last_common_gen": 5, "pc_versions": 3, "metadata_changed": True, "detail": "both changed since gen 5",
                           "action": "Conflict: PS5 and PC changed independently - choose Keep PS5 / Restore PC / Preserve both"},
                          {"user_id": "12229390", "save_name": "sdimg_Save07", "sync_state": "PS5_NEWER", "current_gen": 12,
                           "last_common_gen": 11, "pc_versions": 1, "metadata_changed": False, "detail": "PC download pending; last common gen 11",
                           "action": "PS5 is newer: the PC will pick it up (or Back Up Changed Saves if unprotected)"}]}
    if path == "/api/backup/queue":
        rows = []
        for i, (tid, name, *_r) in enumerate(TITLES[:3] * 3):
            rows.append({"id": "20261007-0%d3000_%s_sdimg_Save0%d_000%d" % (i, tid, i, i), "filename": "x.zip",
                         "title_id": tid, "save_name": "sdimg_Save0%d" % i, "user_id": "12229390", "sha256": "ab" * 32,
                         "size": 7_733_414 * (i + 1), "created_at": NOW - 600 * (9 - i), "reason": "game_close",
                         "state": "ready", "gen_id": i, "lan_ack_at": 0 if i > 5 else NOW - 3600, "cloud_needed": False,
                         "cloud_ack_at": 0, "pc_attempts": 1, "vault_ack_at": 0 if i > 3 else NOW - 3000})
        return {"queue": rows}
    if path == "/api/backup/events":
        ev = [("IDLE_SCAN_COMPLETED", "", "unchanged=30 changed=0 new=0 missing=0 untracked=0 titles_queued=0"),
              ("PC_ACK", "PPSA15508", "id=... save=sdimg_SAVEFILE0P"),
              ("TITLE_BACKUP_DONE", "PPSA15493", "packaged=1 unchanged=3 left=0"),
              ("SAVE_STABLE", "PPSA15493", "after 4003 ms, 3 samples, 0 change(s)"),
              ("GAME_EXIT", "PPSA15493", "Atomfall pid=100"),
              ("SAVE_STABILITY_TIMEOUT", "PPSA24701", "still changing after 45000 ms; not backed up as stable"),
              ("GAME_START", "PPSA15493", "Atomfall app 0x8018 pid=100")]
        out = [{"id": 100 - i, "ts": NOW - 60 * i, "code": c, "title_id": t, "detail": d}
               for i, (c, t, d) in enumerate(ev)]
        if STATE["scan_done_at"] and time.time() >= STATE["scan_done_at"]:
            out.insert(0, {"id": 101, "ts": int(STATE["scan_done_at"]), "code": "IDLE_SCAN_CHANGED_COUNT", "title_id": "",
                           "detail": "0 changed, 0 new save(s); 0 title(s) queued"})
        return {"events": out}
    if path == "/api/settings/sync":
        if method == "POST":
            cfg = cfg_now()
            new = json.loads(body or "{}")
            if new.get("vault_token") == "********":
                new.pop("vault_token")
            cfg.update(new)
            STATE["settings"] = cfg
        shown = dict(cfg_now())
        if shown.get("vault_token"):
            shown["vault_token"] = "********"
        return {"path": "/data/shallot/sync.json", "config": shown}
    if path == "/api/backup/scan_changed":
        STATE["scan_done_at"] = time.time() + 4
        return {"ok": True, "queued": True}
    if path == "/api/backup/protect_title":
        return {"ok": True, "queued": True}
    if path == "/api/reconcile/title/full":
        return {"ok": True, "started": True}
    # minimal upstream stubs so the page initializes
    if path == "/api/users":
        return {"users": [{"uid": "12229390", "name": "Alperen"}], "current": "12229390"}
    if path == "/api/saves":
        return saves_list()
    if path == "/api/backup/label":
        STATE.setdefault("labels", {})[q.get("object", "")] = q.get("label", "")
        return {"ok": True}
    if path == "/api/settings/cloud":
        return {"config": {}}
    if path in ("/api/cloud/status", "/api/cloud/auto_status", "/api/status"):
        return {}
    return {"error": "mock: not implemented", "path": path}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, ctype, data):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_any(self, method):
        u = urllib.parse.urlparse(self.path)
        if u.path in ("/", "/index.html"):
            return self._send(200, "text/html; charset=utf-8", html().encode("utf-8"))
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n).decode("utf-8") if n else ""
        q = dict(urllib.parse.parse_qsl(u.query))
        q_raw = u.query
        res = route(method, u.path, q if u.path != "/api/reconcile/catalogue/result" else q_raw, body) \
            if u.path != "/api/reconcile/catalogue/result" else route(method, u.path, q_raw, body)
        self._send(200, "application/json", json.dumps(res).encode("utf-8"))

    def do_GET(self):
        self.handle_any("GET")

    def do_POST(self):
        self.handle_any("POST")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
    print("UI mock: http://127.0.0.1:%d/" % port)
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
