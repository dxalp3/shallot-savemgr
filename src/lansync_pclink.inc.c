/*
 * LAN save sync — link to the PC client. Included from main.c after
 * lansync_system.inc.c.
 *
 * Delivery stays "PC pulls, PS5 forgets only after a verified ack", but
 * without polling delay: the PC keeps one request open in
 *   GET  /api/backup/wait?seq=<last>&timeout=<s>&client=<name>&version=<v>
 * which returns as soon as an archive is queued or someone asks to sync now
 *   POST /api/backup/kick
 * (or after the timeout). Anything the PC could not take stays queued and
 * goes with the next pass, so a backlog drains when the PC is back.
 *
 * Also: who the PC client is (status card), and the files to set one up:
 *   GET  /api/pc/shallot_sync.py   (the client, embedded at build time)
 *   GET  /api/pc/config.json      (config with this PS5's address filled in)
 *   GET  /api/pc/docker/<file>    (Dockerfile, docker-entrypoint.sh,
 *                                  docker-compose.yml with this PS5's address)
 *   GET  /api/pc/vault/<file>     (the vault: shallot_vault.py, Dockerfile,
 *                                  docker-compose.yml, Caddyfile)
 *
 * Managing the PC client from the web UI (tray, console or Docker alike):
 *   POST /api/pc/command?do=ping|sync|pause|resume|thin|vault|update|quit
 * leaves one command for the client; the open wait request returns at once
 * with "cmd"/"cmd_id", the client runs it and reports
 *   POST /api/pc/command_done?id=<n>   (body: short result text)
 * The UI follows it in /api/pc/status ("cmd"). A newer command replaces one
 * the client has not picked up yet.
 *
 * "PC and VPS" with the PC uploading (vps_route "pc"): the client takes the
 * vault address, token and key from
 *   GET  /api/pc/vault_config
 * unless its own config.json names a vault, and reports what reached it
 *   POST /api/pc/vault_acks   (body {"ids":[...]}; lansync_http.inc.c)
 * so the PS5 keeps each archive until both the PC and the vault have it.
 *
 * One PC client collects. Every client sends cid=<its id> (older ones are
 * known by address); the first that connects becomes the active one
 * (sync.json pc_active), others are on standby and get nothing: no queue,
 * no restore points, no commands, no vault settings, and their acks are
 * refused. The web UI lists them and can switch or block them:
 *   GET  /api/pc/clients
 *   POST /api/pc/clients?id=<cid>&do=activate|block|unblock|forget
 */

#ifndef LS_HOST_TEST
#include "pc_client.h"
#endif

typedef struct {
    char name[64], version[24], addr[48];
    char vault[16];              /* PC's off-site copy: off | ok | pending | error | starting */
    int64_t vault_pending, vault_at;
    char vault_src[8];           /* where its vault settings come from: own | ps5 | "" */
    char kind[12];               /* tray | console | docker */
    char caps[64];               /* commands it understands, comma separated */
    int paused;                  /* collecting paused from the web UI */
    int64_t held, held_bytes, free_bytes;   /* versions on the PC, its store size, free disk */
    int64_t last_seen;
    int waiting;                 /* requests currently held in /api/backup/wait */
} ls_pc_t;

/* One command for the PC client (see the header comment). */
typedef struct {
    int id;
    char what[12];
    int64_t at, picked_at, done_at;
    int ok;
    char result[200];
} ls_pc_cmd_t;
static ls_pc_cmd_t g_ls_pc_cmd;
static int g_ls_pc_cmd_next = 1;
#define LS_PC_CMD_TTL_S 300      /* a command nobody picked up expires */

/* Last time the PC client was seen (since this Shallot start). */
static int64_t g_ls_last_pc_poll = 0;

static pthread_mutex_t g_ls_pc_mtx = PTHREAD_MUTEX_INITIALIZER;
static ls_pc_t g_ls_pc;

#define LS_PC_CONNECTED_S 90     /* "connected" if seen this recently */
#define LS_PC_LEASE_S 1800       /* the primary client away this long: a connected one stands in */
static int64_t g_ls_pc_gate_since;   /* first PC request since Shallot started */
static char g_ls_standin[40];        /* client collecting copies while the primary is away ("" = none) */
static int64_t g_ls_standin_seen;

/* PC clients seen since start (the registry; who is active lives in sync.json). */
#define LS_PC_PEERS 8
typedef struct {
    char id[40], name[64], kind[12], version[24], addr[48];
    int64_t first_seen, last_seen;
} ls_pc_peer_t;
static ls_pc_peer_t g_ls_peers[LS_PC_PEERS];

/* Value of query parameter `key` ("&key=" or "?key="), decoded; 1 if present. */
static int ls_qparam(const char *url, const char *key, char *out, size_t out_sz) {
    char pat[40];
    out[0] = 0;
    for (const char *sep = "?&"; *sep; sep++) {
        snprintf(pat, sizeof(pat), "%c%s=", *sep, key);
        const char *p = strstr(url, pat);
        if (p) { url_param_decode(p + strlen(pat), out, out_sz); return 1; }
    }
    return 0;
}

static void ls_keep_word(char *s, const char *extra) {
    for (char *c = s; *c; c++) if (!isalnum((unsigned char)*c) && !strchr(extra, *c)) *c = '_';
}

static void ls_peer_addr(int sock, char *addr, size_t addr_sz) {
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    addr[0] = 0;
    if (sock >= 0 && getpeername(sock, (struct sockaddr *)&peer, &plen) == 0)
        inet_ntop(AF_INET, &peer.sin_addr, addr, addr_sz);
}

/* Is `id` in the comma separated list? */
static int ls_id_listed(const char *list, const char *id) {
    size_t n = strlen(id);
    for (const char *p = list; n && *p;) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && strncmp(p, id, n) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

static void ls_id_list_remove(char *list, const char *id) {
    char out[200] = "";
    for (char *p = list; *p;) {
        char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (!(l == strlen(id) && strncmp(p, id, l) == 0) && l)
            snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s%.*s", out[0] ? "," : "", (int)l, p);
        if (!e) break;
        p = e + 1;
    }
    snprintf(list, 200, "%s", out);
}

/*
 * Records the PC client behind a request and returns its role:
 *   1 the primary client (sync.json pc_active; the first one not blocked
 *     becomes primary when none is set): the PS5 forgets an archive only
 *     after the primary has it, so the primary always ends up with all of them;
 *   2 the stand-in: the primary has been away for LS_PC_LEASE_S, this client
 *     is here and collects copies meanwhile (the PS5 keeps them for the
 *     primary, which collects what it missed when it is back);
 *   0 standby / blocked: gets nothing.
 */
static int ls_pc_decide(const char *id, const char *addr, const char *kind, const char *name, int64_t now);
static int ls_pc_gate(int sock, const char *url, char *id_out, size_t id_sz) {
    char id[40] = "", addr[48] = "", name[64] = "", kind[12] = "", ver[24] = "";
    ls_peer_addr(sock, addr, sizeof(addr));
    ls_qparam(url, "cid", id, sizeof(id));
    ls_keep_word(id, "-");
    if (!id[0]) snprintf(id, sizeof(id), "ip:%s", addr[0] ? addr : "unknown");    /* clients before 2.2 */
    ls_keep_word(id, ":._-");                      /* as sync.json stores it */
    ls_qparam(url, "client", name, sizeof(name));
    ls_qparam(url, "kind", kind, sizeof(kind));
    ls_qparam(url, "version", ver, sizeof(ver));
    ls_keep_word(kind, "");
    ls_keep_word(ver, ".-");
    int64_t now = (int64_t)time(NULL);
    pthread_mutex_lock(&g_ls_pc_mtx);
    if (!g_ls_pc_gate_since) g_ls_pc_gate_since = now;
    int slot = -1, oldest = 0;
    for (int i = 0; i < LS_PC_PEERS; i++) {
        if (strcmp(g_ls_peers[i].id, id) == 0) { slot = i; break; }
        if (g_ls_peers[i].last_seen < g_ls_peers[oldest].last_seen) oldest = i;
    }
    if (slot < 0) {
        slot = oldest;
        memset(&g_ls_peers[slot], 0, sizeof(g_ls_peers[slot]));
        snprintf(g_ls_peers[slot].id, sizeof(g_ls_peers[slot].id), "%s", id);
        g_ls_peers[slot].first_seen = now;
    }
    ls_pc_peer_t *pe = &g_ls_peers[slot];
    pe->last_seen = now;
    if (addr[0]) snprintf(pe->addr, sizeof(pe->addr), "%s", addr);
    if (name[0]) snprintf(pe->name, sizeof(pe->name), "%s", name);
    if (kind[0]) snprintf(pe->kind, sizeof(pe->kind), "%s", kind);
    if (ver[0]) snprintf(pe->version, sizeof(pe->version), "%s", ver);
    pthread_mutex_unlock(&g_ls_pc_mtx);
    if (id_out) snprintf(id_out, id_sz, "%s", id);

    pthread_mutex_lock(&g_ls_cfg_mtx);
    int role = ls_pc_decide(id, addr, kind, name, now);
    pthread_mutex_unlock(&g_ls_cfg_mtx);
    return role;
}

/* ls_pc_gate's decision; called with g_ls_cfg_mtx held (it may write sync.json). */
static int ls_pc_decide(const char *id, const char *addr, const char *kind, const char *name, int64_t now) {
    ls_config_t cfg;
    ls_load_config(&cfg);
    if (ls_id_listed(cfg.pc_blocked, id)) return 0;
    /* an updated client on the same machine replaces the old, address-only entry
       (not a container: it shares the host's address but is another client) */
    char old_id[60];
    snprintf(old_id, sizeof(old_id), "ip:%s", addr);
    ls_keep_word(old_id, ":._-");
    if (addr[0] && strncmp(id, "ip:", 3) != 0 && strcmp(kind, "docker") != 0 && strcmp(cfg.pc_active, old_id) == 0) {
        snprintf(cfg.pc_active, sizeof(cfg.pc_active), "%s", id);
        if (ls_save_config(&cfg) == 0)
            ls_event("PC_CLIENT_ACTIVE", NULL, "%s replaces the older client at %s", id, addr);
    }
    /* the primary has been away too long: a client that is here stands in */
    if (cfg.pc_active[0] && strcmp(cfg.pc_active, id) != 0) {
        int64_t act_seen = g_ls_pc_gate_since;          /* not seen since Shallot started: count from then */
        int newly = 0, take = 0;
        pthread_mutex_lock(&g_ls_pc_mtx);
        for (int i = 0; i < LS_PC_PEERS; i++)
            if (strcmp(g_ls_peers[i].id, cfg.pc_active) == 0) act_seen = g_ls_peers[i].last_seen;
        if (now - act_seen > LS_PC_LEASE_S) {
            take = !g_ls_standin[0] || strcmp(g_ls_standin, id) == 0 || now - g_ls_standin_seen > 3 * LS_PC_CONNECTED_S;
            if (take) {
                newly = strcmp(g_ls_standin, id) != 0;
                snprintf(g_ls_standin, sizeof(g_ls_standin), "%s", id);
                g_ls_standin_seen = now;
            }
        }
        pthread_mutex_unlock(&g_ls_pc_mtx);
        if (newly)
            ls_event("PC_CLIENT_STANDIN", NULL, "%s collects copies while %s is away (%lld min); the PS5 keeps everything for it",
                     id, cfg.pc_active, (long long)((now - act_seen) / 60));
        if (take) return 2;
    }
    if (!cfg.pc_active[0]) {
        snprintf(cfg.pc_active, sizeof(cfg.pc_active), "%s", id);
        if (ls_save_config(&cfg) == 0)
            ls_event("PC_CLIENT_ACTIVE", NULL, "%s (%s, %s) now collects the backups", id, name[0] ? name : "?", addr);
    }
    if (strcmp(cfg.pc_active, id) != 0) return 0;
    pthread_mutex_lock(&g_ls_pc_mtx);
    g_ls_standin[0] = 0;                           /* the primary is back */
    pthread_mutex_unlock(&g_ls_pc_mtx);
    return 1;
}

/* Records the PC client from a request (client=, version=, peer address). */
static void ls_pc_seen(int sock, const char *url) {
    char name[64] = "", ver[24] = "", addr[48] = "", vault[16] = "", vp[24] = "", va[24] = "";
    char vsrc[8] = "", kind[12] = "", caps[64] = "", paused[8] = "", held[24] = "", hb[24] = "", fb[24] = "";
    ls_qparam(url, "client", name, sizeof(name));
    ls_qparam(url, "version", ver, sizeof(ver));
    ls_qparam(url, "vault", vault, sizeof(vault));
    ls_qparam(url, "vault_pending", vp, sizeof(vp));
    ls_qparam(url, "vault_at", va, sizeof(va));
    ls_qparam(url, "vault_src", vsrc, sizeof(vsrc));
    ls_qparam(url, "kind", kind, sizeof(kind));
    ls_qparam(url, "caps", caps, sizeof(caps));
    int have_paused = ls_qparam(url, "paused", paused, sizeof(paused));
    int have_held = ls_qparam(url, "held", held, sizeof(held));
    ls_qparam(url, "held_bytes", hb, sizeof(hb));
    ls_qparam(url, "free", fb, sizeof(fb));
    for (char *c = vault; *c; c++) if (!isalpha((unsigned char)*c)) *c = '_';
    ls_keep_word(vsrc, "");
    ls_keep_word(kind, "");
    ls_keep_word(caps, ",");
    ls_keep_word(ver, ".-");
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    if (sock >= 0 && getpeername(sock, (struct sockaddr *)&peer, &plen) == 0)
        inet_ntop(AF_INET, &peer.sin_addr, addr, sizeof(addr));
    pthread_mutex_lock(&g_ls_pc_mtx);
    if (name[0]) snprintf(g_ls_pc.name, sizeof(g_ls_pc.name), "%s", name);
    if (ver[0]) snprintf(g_ls_pc.version, sizeof(g_ls_pc.version), "%s", ver);
    if (addr[0]) snprintf(g_ls_pc.addr, sizeof(g_ls_pc.addr), "%s", addr);
    if (vault[0]) {
        snprintf(g_ls_pc.vault, sizeof(g_ls_pc.vault), "%s", vault);
        g_ls_pc.vault_pending = strtoll(vp, NULL, 10);
        g_ls_pc.vault_at = strtoll(va, NULL, 10);
        snprintf(g_ls_pc.vault_src, sizeof(g_ls_pc.vault_src), "%s", vsrc);
    }
    if (kind[0]) snprintf(g_ls_pc.kind, sizeof(g_ls_pc.kind), "%s", kind);
    if (kind[0]) snprintf(g_ls_pc.caps, sizeof(g_ls_pc.caps), "%s", caps);
    if (have_paused) g_ls_pc.paused = atoi(paused) != 0;
    if (have_held) {
        g_ls_pc.held = strtoll(held, NULL, 10);
        g_ls_pc.held_bytes = strtoll(hb, NULL, 10);
        g_ls_pc.free_bytes = strtoll(fb, NULL, 10);
    }
    g_ls_pc.last_seen = (int64_t)time(NULL);
    pthread_mutex_unlock(&g_ls_pc_mtx);
    g_ls_last_pc_poll = g_ls_pc.last_seen;
}

/* Any PC client request (download, ack, manifest) counts as contact. */
static void ls_pc_touch(void) {
    pthread_mutex_lock(&g_ls_pc_mtx);
    g_ls_pc.last_seen = (int64_t)time(NULL);
    pthread_mutex_unlock(&g_ls_pc_mtx);
    g_ls_last_pc_poll = g_ls_pc.last_seen;
}

/* The collecting client's requests count as contact; returns its role (see ls_pc_gate). */
static int ls_pc_role(int sock, const char *url, char *id_out, size_t id_sz) {
    int role = ls_pc_gate(sock, url, id_out, id_sz);
    if (role) ls_pc_touch();
    return role;
}
static int ls_pc_active_req(int sock, const char *url) { return ls_pc_role(sock, url, NULL, 0); }
static int ls_pc_primary_req(int sock, const char *url) { return ls_pc_role(sock, url, NULL, 0) == 1; }

/*
 * Blocks until the wait sequence differs from `seq` or `timeout_s` passes.
 * seq <= 0 returns at once (first call). Returns the current sequence;
 * *reason: "new", "kick", "start" or "timeout".
 */
static int64_t ls_wait_for(int64_t seq, int timeout_s, char *reason, size_t reason_sz) {
    if (timeout_s < 0) timeout_s = 0;
    if (timeout_s > 60) timeout_s = 60;
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += timeout_s;
    pthread_mutex_lock(&g_ls_wait_mtx);
    while (seq > 0 && g_ls_wait_seq == seq) {
        if (pthread_cond_timedwait(&g_ls_wait_cv, &g_ls_wait_mtx, &dl) == ETIMEDOUT) break;
    }
    int64_t cur = g_ls_wait_seq;
    snprintf(reason, reason_sz, "%s", cur == seq ? "timeout" : g_ls_wait_reason);
    pthread_mutex_unlock(&g_ls_wait_mtx);
    return cur;
}

/* The client version embedded in this build (CLIENT_VERSION = "x.y"), or "". */
static void ls_pc_bundled_version(char *out, size_t out_sz) {
    out[0] = 0;
#ifndef LS_HOST_TEST
    static const char key[] = "CLIENT_VERSION = \"";
    size_t kl = sizeof(key) - 1, n = pc_shallot_sync_py_len;
    for (size_t i = 0; i + kl < n; i++) {
        if (memcmp(pc_shallot_sync_py + i, key, kl) != 0) continue;
        size_t j = 0;
        for (i += kl; i < n && pc_shallot_sync_py[i] != '"' && j + 1 < out_sz; i++) out[j++] = (char)pc_shallot_sync_py[i];
        out[j] = 0;
        return;
    }
#else
    snprintf(out, out_sz, "%s", "9.9");
#endif
}

static int ls_pc_cmd_known(const char *w) {
    static const char *const ok[] = { "ping", "sync", "pause", "resume", "thin", "vault", "update", "quit", NULL };
    for (int i = 0; ok[i]; i++) if (strcmp(w, ok[i]) == 0) return 1;
    return 0;
}

/* Leaves a command for the PC client. Returns its id, or -1 (unknown). */
static int ls_pc_command(const char *what) {
    if (!ls_pc_cmd_known(what)) return -1;
    pthread_mutex_lock(&g_ls_pc_mtx);
    memset(&g_ls_pc_cmd, 0, sizeof(g_ls_pc_cmd));
    g_ls_pc_cmd.id = g_ls_pc_cmd_next++;
    snprintf(g_ls_pc_cmd.what, sizeof(g_ls_pc_cmd.what), "%s", what);
    g_ls_pc_cmd.at = (int64_t)time(NULL);
    int id = g_ls_pc_cmd.id;
    pthread_mutex_unlock(&g_ls_pc_mtx);
    ls_wait_bump("cmd");
    return id;
}

/* The client reported a command as done. */
static int ls_pc_command_done(int id, int ok, const char *result) {
    pthread_mutex_lock(&g_ls_pc_mtx);
    int hit = id > 0 && g_ls_pc_cmd.id == id && !g_ls_pc_cmd.done_at;
    if (hit) {
        g_ls_pc_cmd.done_at = (int64_t)time(NULL);
        g_ls_pc_cmd.ok = ok;
        snprintf(g_ls_pc_cmd.result, sizeof(g_ls_pc_cmd.result), "%s", result ? result : "");
    }
    pthread_mutex_unlock(&g_ls_pc_mtx);
    return hit;
}

/* ,"cmd":"x","cmd_id":n for a wait answer when a command is waiting (marks it picked up). */
static void ls_pc_command_take(char *out, size_t out_sz) {
    int64_t now = (int64_t)time(NULL);
    out[0] = 0;
    pthread_mutex_lock(&g_ls_pc_mtx);
    if (g_ls_pc_cmd.id && !g_ls_pc_cmd.done_at && now - g_ls_pc_cmd.at < LS_PC_CMD_TTL_S) {
        snprintf(out, out_sz, ",\"cmd\":\"%s\",\"cmd_id\":%d", g_ls_pc_cmd.what, g_ls_pc_cmd.id);
        if (!g_ls_pc_cmd.picked_at) g_ls_pc_cmd.picked_at = now;
    }
    pthread_mutex_unlock(&g_ls_pc_mtx);
}

static int64_t ls_pc_pending(int64_t *bytes) {
    int64_t n = 0;
    if (bytes) *bytes = 0;
    ls_config_t cfg;
    ls_load_config(&cfg);
    if (!cfg.storage_pc) return 0;                 /* nothing is meant for a PC in this mode */
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "SELECT COUNT(*), COALESCE(SUM(size),0) FROM queue WHERE state IN ('ready','packed') AND lan_ack_at = 0",
                               -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
            n = sqlite3_column_int64(st, 0);
            if (bytes) *bytes = sqlite3_column_int64(st, 1);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    return n;
}

/* "pc":{...} object for the status JSON. */
static void ls_pc_json(char *out, size_t out_sz) {
    pthread_mutex_lock(&g_ls_pc_mtx);
    ls_pc_t pc = g_ls_pc;
    pthread_mutex_unlock(&g_ls_pc_mtx);
    int64_t now = (int64_t)time(NULL), bytes = 0;
    int64_t pending = ls_pc_pending(&bytes);
    int connected = pc.waiting > 0 || (pc.last_seen && now - pc.last_seen < LS_PC_CONNECTED_S);
    pthread_mutex_lock(&g_ls_pc_mtx);
    ls_pc_cmd_t cmd = g_ls_pc_cmd;
    pthread_mutex_unlock(&g_ls_pc_mtx);
    char en[140], ev[60], bundled[24], cres[420], cj[600] = "";
    json_escape(pc.name, en, sizeof(en));
    json_escape(pc.version, ev, sizeof(ev));
    ls_pc_bundled_version(bundled, sizeof(bundled));
    if (cmd.id) {
        const char *cst = cmd.done_at ? (cmd.ok ? "done" : "failed")
                        : now - cmd.at >= LS_PC_CMD_TTL_S ? "expired" : cmd.picked_at ? "running" : "waiting";
        json_escape(cmd.result, cres, sizeof(cres));
        snprintf(cj, sizeof(cj), ",\"cmd\":{\"id\":%d,\"do\":\"%s\",\"state\":\"%s\",\"at\":%lld,\"done_at\":%lld,\"result\":\"%s\"}",
                 cmd.id, cmd.what, cst, (long long)cmd.at, (long long)cmd.done_at, cres);
    }
    int standby = 0;
    char act_id[40] = "";
    {
        ls_config_t cfg;
        ls_load_config(&cfg);
        snprintf(act_id, sizeof(act_id), "%s", cfg.pc_active);
        pthread_mutex_lock(&g_ls_pc_mtx);
        for (int i = 0; i < LS_PC_PEERS; i++)
            if (g_ls_peers[i].id[0] && strcmp(g_ls_peers[i].id, act_id) && now - g_ls_peers[i].last_seen < LS_PC_CONNECTED_S) standby++;
        pthread_mutex_unlock(&g_ls_pc_mtx);
    }
    snprintf(out, out_sz,
             "{\"id\":\"%s\",\"standby\":%d,\"connected\":%s,\"live\":%s,\"name\":\"%s\",\"version\":\"%s\",\"bundled_version\":\"%s\",\"addr\":\"%s\",\"last_seen\":%lld,"
             "\"kind\":\"%s\",\"caps\":\"%s\",\"paused\":%s,\"held\":%lld,\"held_bytes\":%lld,\"free_bytes\":%lld,"
             "\"backlog\":%lld,\"backlog_bytes\":%lld,\"vault\":{\"state\":\"%s\",\"pending\":%lld,\"at\":%lld,\"src\":\"%s\"}%s}",
             act_id, standby, connected ? "true" : "false", pc.waiting > 0 ? "true" : "false", en, ev, bundled, pc.addr,
             (long long)pc.last_seen, pc.kind, pc.caps, pc.paused ? "true" : "false", (long long)pc.held,
             (long long)pc.held_bytes, (long long)pc.free_bytes, (long long)pending, (long long)bytes,
             pc.vault[0] ? pc.vault : "off", (long long)pc.vault_pending, (long long)pc.vault_at, pc.vault_src, cj);
}

static int (*g_ls_vault_key_hex)(char hex[65]);      /* set by lansync_vault.inc.c */

/* {"route":...} and, for "PC and VPS" with the PC uploading, what it needs. */
static void ls_pc_vault_config_json(char *out, size_t out_sz) {
    ls_config_t cfg;
    ls_load_config(&cfg);
    if (!cfg.vps_via_pc) {
        snprintf(out, out_sz, "{\"route\":\"%s\"}", cfg.storage_vps ? "ps5" : "off");
        return;
    }
    char hex[65] = "", eu[340], et[220];
    if (!cfg.vault_url[0] || !cfg.vault_token[0] || !g_ls_vault_key_hex || g_ls_vault_key_hex(hex) < 0) {
        snprintf(out, out_sz, "{\"route\":\"pc\",\"error\":\"the vault address or token is not set on the PS5\"}");
        return;
    }
    json_escape(cfg.vault_url, eu, sizeof(eu));
    json_escape(cfg.vault_token, et, sizeof(et));
    snprintf(out, out_sz, "{\"route\":\"pc\",\"url\":\"%s\",\"token\":\"%s\",\"key\":\"%s\"}", eu, et, hex);
}

/* This PS5's address as the requesting PC reached it. */
static void ls_local_addr(int sock, char *out, size_t out_sz) {
    struct sockaddr_in me;
    socklen_t len = sizeof(me);
    out[0] = 0;
    if (getsockname(sock, (struct sockaddr *)&me, &len) == 0) inet_ntop(AF_INET, &me.sin_addr, out, out_sz);
    if (!out[0] || strcmp(out, "127.0.0.1") == 0) snprintf(out, out_sz, "%s", "192.168.1.50");
}

/* Copy of src (n bytes) with the first `need` replaced by `repl` (NUL-terminated, caller frees). */
static char *ls_replace_once(const char *src, size_t n, const char *need, const char *repl, size_t *out_n) {
    size_t nl = strlen(need), rl = strlen(repl), at = n;
    for (size_t i = 0; i + nl <= n; i++)
        if (memcmp(src + i, need, nl) == 0) { at = i; break; }
    if (at == n) { nl = rl = 0; }
    char *out = malloc(n - nl + rl + 1);
    if (!out) return NULL;
    memcpy(out, src, at);
    memcpy(out + at, repl, rl);
    memcpy(out + at + rl, src + at + nl, n - at - nl);
    *out_n = n - nl + rl;
    out[*out_n] = 0;
    return out;
}

static void ls_send_attachment(int sock, const char *ctype, const char *fname, const void *data, size_t len) {
    char hdr[512];
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Disposition: attachment; filename=\"%s\"\r\n"
                      "Content-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n"
                      "Connection: close\r\n\r\n", ctype, fname, len);
    send(sock, hdr, hl, 0);
    size_t off = 0;
    while (off < len) {
        ssize_t s = send(sock, (const char *)data + off, len - off, 0);
        if (s <= 0) break;
        off += (size_t)s;
    }
}

/*
 * Before /api/shutdown stops the process: start nothing new (same rules as
 * a console power-off past its budget), let a running backup or restore
 * finish, at most max_s seconds. Returns the seconds waited.
 */
static int ls_stop_prepare(int max_s) {
    if (!g_ls_worker_started) return 0;
    ls_power_set(LS_PWR_SIGNALLED, (int64_t)time(NULL) - LS_PWR_BUDGET_S);
    int waited = 0;
    for (;; waited++) {
        pthread_mutex_lock(&g_ls_job_mtx);
        int busy = g_ls_worker_current[0] != 0;
        pthread_mutex_unlock(&g_ls_job_mtx);
        pthread_mutex_lock(&g_ls_restore_mtx);
        int restoring = g_ls_restore.active && strcmp(g_ls_restore.state, "queued") != 0;
        pthread_mutex_unlock(&g_ls_restore_mtx);
        if ((!busy && !restoring) || waited >= max_s) break;
        sleep(1);
    }
    ls_event("SHALLOT_STOP", NULL, "stopped from the web UI/API after waiting %d s for running work", waited);
    return waited;
}

/* ── Getting archives back from the PC ─────────────────────────────
 * A version whose archives were pruned from the PS5 (the PC has them) can
 * still be restored: the web UI asks for them (POST /api/pc/fetch), the PC
 * client is woken (reason "fetch"), lists what is wanted
 * (GET /api/pc/requests) and uploads each archive
 * (POST /api/pc/readopt?id=<queue id>, lansync_http.inc.c). Shallot checks it
 * against the SHA-256 recorded when it made the archive and puts it back
 * into the queue; then the normal restore runs. Requests live in memory.
 */
#define LS_FETCH_MAX 8
#define LS_FETCH_FILES 64
#define LS_FETCH_TIMEOUT_S 120
typedef struct {
    int id;
    int64_t snap_id, created_at, updated_at;
    char object_id[256];
    int n, got;
    char qid[LS_FETCH_FILES][200];
    char done[LS_FETCH_FILES];
    char error[160];
} ls_fetch_t;
static pthread_mutex_t g_ls_fetch_mtx = PTHREAD_MUTEX_INITIALIZER;
static ls_fetch_t g_ls_fetch[LS_FETCH_MAX];
static int g_ls_fetch_next = 1;

static const char *ls_fetch_state_locked(const ls_fetch_t *f, int64_t now) {
    if (f->error[0]) return "failed";
    if (f->got >= f->n) return "ready";
    if (now - f->updated_at > LS_FETCH_TIMEOUT_S) return "failed";
    return f->got ? "receiving" : "waiting";
}

static void (*g_ls_vault_fetch_start)(int fetch_id);   /* set by lansync_vault.inc.c */

/* Archive ids of a snapshot (one save object, or all) that are not on the
 * PS5 but on the PC. Returns the request id (> 0), 0 = nothing missing,
 * < 0 = error (err set). */
static int ls_fetch_request(int64_t snap_id, const char *object_id, char *err, size_t err_sz) {
    ls_snap_file_t *f = NULL;
    int n = ls_snapshot_files(snap_id, &f);
    if (n <= 0) { free(f); snprintf(err, err_sz, "unknown restore point"); return -1; }
    ls_fetch_t r;
    memset(&r, 0, sizeof(r));
    for (int i = 0; i < n; i++) {
        if (object_id && object_id[0] && strcmp(f[i].object_id, object_id) != 0) continue;
        ls_queue_row_t q;
        if (!f[i].queue_id[0] || !ls_queue_get(f[i].queue_id, &q)) {
            free(f); snprintf(err, err_sz, "%s: no archive recorded for this version", f[i].save_name); return -1;
        }
        if (strcmp(q.state, "ready") == 0 || strcmp(q.state, "packed") == 0) continue;   /* still on the PS5 */
        if (q.lan_ack_at == 0) {
            pthread_mutex_lock(&g_ls_mtx);
            int64_t vack = ls_db_int_locked("SELECT CASE WHEN vault_gone = 0 THEN vault_ack_at ELSE 0 END FROM queue"
                                            " WHERE id = ? AND ? = ? AND 0 * ? = 0", q.id, "x", "x", 0);
            pthread_mutex_unlock(&g_ls_mtx);
            if (vack <= 0) { free(f); snprintf(err, err_sz, "%s: this archive never reached the PC or the vault", f[i].save_name); return -1; }
        }
        if (r.n >= LS_FETCH_FILES) { free(f); snprintf(err, err_sz, "too many files"); return -1; }
        snprintf(r.qid[r.n++], sizeof(r.qid[0]), "%s", q.id);
    }
    free(f);
    if (r.n == 0) return 0;
    int64_t now = (int64_t)time(NULL);
    pthread_mutex_lock(&g_ls_fetch_mtx);
    int slot = 0;                                   /* reuse a finished or the oldest slot */
    for (int i = 0; i < LS_FETCH_MAX; i++) {
        if (!g_ls_fetch[i].id) { slot = i; break; }
        if (g_ls_fetch[i].updated_at < g_ls_fetch[slot].updated_at) slot = i;
    }
    r.id = g_ls_fetch_next++;
    r.snap_id = snap_id;
    snprintf(r.object_id, sizeof(r.object_id), "%s", object_id ? object_id : "");
    r.created_at = r.updated_at = now;
    g_ls_fetch[slot] = r;
    pthread_mutex_unlock(&g_ls_fetch_mtx);
    ls_event("PC_FETCH_REQUESTED", NULL, "restore point #%lld%s%s: %d archive(s) asked from the PC", (long long)snap_id,
             object_id && object_id[0] ? " save " : "", object_id ? object_id : "", r.n);
    ls_wait_bump("fetch");
    {
        ls_config_t cfg;
        ls_load_config(&cfg);
        if (cfg.storage_vps && g_ls_vault_fetch_start) g_ls_vault_fetch_start(r.id);    /* straight from the vault; a PC may answer too */
    }
    return r.id;
}

/* An archive came back: tick it off in every request that wanted it. */
static void ls_fetch_arrived(const char *qid) {
    int64_t now = (int64_t)time(NULL);
    pthread_mutex_lock(&g_ls_fetch_mtx);
    for (int i = 0; i < LS_FETCH_MAX; i++) {
        ls_fetch_t *f = &g_ls_fetch[i];
        if (!f->id) continue;
        for (int k = 0; k < f->n; k++)
            if (!f->done[k] && strcmp(f->qid[k], qid) == 0) { f->done[k] = 1; f->got++; f->updated_at = now; f->error[0] = 0; }
    }
    pthread_mutex_unlock(&g_ls_fetch_mtx);
}

static void ls_fetch_failed(const char *qid, const char *why) {
    pthread_mutex_lock(&g_ls_fetch_mtx);
    for (int i = 0; i < LS_FETCH_MAX; i++) {
        ls_fetch_t *f = &g_ls_fetch[i];
        if (!f->id || f->error[0]) continue;
        for (int k = 0; k < f->n; k++)
            if (!f->done[k] && strcmp(f->qid[k], qid) == 0) snprintf(f->error, sizeof(f->error), "%s", why);
    }
    pthread_mutex_unlock(&g_ls_fetch_mtx);
}

/* {"requests":[{"id","sha256","size"}]}: archives the PC should upload now. */
static void ls_fetch_list_json(char **buf, size_t *cap, size_t *pos) {
    int64_t now = (int64_t)time(NULL);
    char want[LS_FETCH_MAX * LS_FETCH_FILES][200];
    int nw = 0;
    pthread_mutex_lock(&g_ls_fetch_mtx);
    for (int i = 0; i < LS_FETCH_MAX; i++) {
        ls_fetch_t *f = &g_ls_fetch[i];
        if (!f->id || strcmp(ls_fetch_state_locked(f, now), "failed") == 0) continue;
        for (int k = 0; k < f->n; k++) {
            if (f->done[k]) continue;
            int dup = 0;
            for (int j = 0; j < nw && !dup; j++) dup = strcmp(want[j], f->qid[k]) == 0;
            if (!dup) snprintf(want[nw++], sizeof(want[0]), "%s", f->qid[k]);
        }
    }
    pthread_mutex_unlock(&g_ls_fetch_mtx);
    ls_json_append(buf, cap, pos, "{\"requests\":[");
    for (int j = 0; j < nw; j++) {
        ls_queue_row_t q;
        if (!ls_queue_get(want[j], &q)) continue;
        char eid[420];
        json_escape(q.id, eid, sizeof(eid));
        ls_json_append(buf, cap, pos, "%s{\"id\":\"%s\",\"sha256\":\"%s\",\"size\":%lld}", j ? "," : "", eid, q.sha256,
                       (long long)q.size);
    }
    ls_json_append(buf, cap, pos, "]}");
}

static void ls_fetch_status_json(int id, char *out, size_t out_sz) {
    int64_t now = (int64_t)time(NULL);
    pthread_mutex_lock(&g_ls_fetch_mtx);
    const ls_fetch_t *f = NULL;
    for (int i = 0; i < LS_FETCH_MAX; i++) if (g_ls_fetch[i].id == id) f = &g_ls_fetch[i];
    if (!f) snprintf(out, out_sz, "{\"error\":\"unknown request\"}");
    else {
        const char *st = ls_fetch_state_locked(f, now);
        char ee[340];
        json_escape(f->error[0] ? f->error : (strcmp(st, "failed") == 0 ? "the PC did not send the archives in time" : ""),
                    ee, sizeof(ee));
        snprintf(out, out_sz, "{\"id\":%d,\"state\":\"%s\",\"got\":%d,\"n\":%d,\"error\":\"%s\"}", f->id, st, f->got, f->n, ee);
    }
    pthread_mutex_unlock(&g_ls_fetch_mtx);
}

/* Routes of this file; 1 = handled. */
static int ls_pclink_route(int sock, const char *method, const char *url) {
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/backup/wait", 16) == 0) {
        int64_t seq = 0;
        int timeout = 25;
        const char *p;
        if ((p = strstr(url, "seq="))) seq = strtoll(p + 4, NULL, 10);
        if ((p = strstr(url, "timeout="))) timeout = atoi(p + 8);
        int active = ls_pc_gate(sock, url, NULL, 0);
        if (active) {
            ls_pc_seen(sock, url);
            pthread_mutex_lock(&g_ls_pc_mtx);
            g_ls_pc.waiting++;
            pthread_mutex_unlock(&g_ls_pc_mtx);
        }
        char reason[16];
        int64_t cur = ls_wait_for(seq, timeout, reason, sizeof(reason));
        char j[256], cmd[64] = "";
        int64_t pending = 0;
        if (active) {
            pthread_mutex_lock(&g_ls_pc_mtx);
            g_ls_pc.waiting--;
            g_ls_pc.last_seen = (int64_t)time(NULL);
            pthread_mutex_unlock(&g_ls_pc_mtx);
            pending = ls_pc_pending(NULL);
            ls_pc_command_take(cmd, sizeof(cmd));
        }
        snprintf(j, sizeof(j), "{\"seq\":%lld,\"reason\":\"%s\",\"pending\":%lld%s%s}", (long long)cur, reason,
                 (long long)pending, cmd, active ? "" : ",\"standby\":true");
        http_json(sock, j);
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/pc/command?", 16) == 0) {
        char what[16] = "", j[160];
        ls_qparam(url, "do", what, sizeof(what));
        int id = ls_pc_command(what);
        pthread_mutex_lock(&g_ls_pc_mtx);
        int live = g_ls_pc.waiting > 0;
        pthread_mutex_unlock(&g_ls_pc_mtx);
        if (id < 0) snprintf(j, sizeof(j), "{\"error\":\"unknown command\"}");
        else snprintf(j, sizeof(j), "{\"ok\":true,\"id\":%d,\"pc_connected\":%s}", id, live ? "true" : "false");
        http_json(sock, j);
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/pc/command_done?", 21) == 0) {
        /* the result text travels in the query (the client keeps it short) */
        char ids[16] = "", oks[8] = "", res[200] = "";
        ls_qparam(url, "id", ids, sizeof(ids));
        ls_qparam(url, "ok", oks, sizeof(oks));
        ls_qparam(url, "result", res, sizeof(res));
        if (!ls_pc_active_req(sock, url)) { http_json(sock, "{\"ok\":false}"); return 1; }
        int hit = ls_pc_command_done(atoi(ids), oks[0] != '0', res);
        http_json(sock, hit ? "{\"ok\":true}" : "{\"ok\":false}");
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/pc/vault_config", 20) == 0 && (url[20] == 0 || url[20] == '?')) {
        char j[700];
        if (!ls_pc_primary_req(sock, url)) { http_json(sock, "{\"route\":\"off\",\"standby\":true}"); return 1; }
        ls_pc_vault_config_json(j, sizeof(j));
        http_json(sock, j);
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/backup/kick") == 0) {
        ls_wait_bump("kick");
        pthread_mutex_lock(&g_ls_pc_mtx);
        int live = g_ls_pc.waiting > 0;
        pthread_mutex_unlock(&g_ls_pc_mtx);
        http_json(sock, live ? "{\"ok\":true,\"pc\":\"notified\"}" : "{\"ok\":true,\"pc\":\"not connected\"}");
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/pc/fetch?", 14) == 0) {
        char ids[32] = {0}, obj[256] = {0}, err[200] = {0};
        const char *p;
        if ((p = strstr(url, "snap="))) url_param_decode(p + 5, ids, sizeof(ids));
        if ((p = strstr(url, "object="))) url_param_decode(p + 7, obj, sizeof(obj));
        if (atoll(ids) <= 0 || strchr(obj, '/') || strstr(obj, "..")) { http_json(sock, "{\"error\":\"Invalid request\"}"); return 1; }
        int id = ls_fetch_request(atoll(ids), obj, err, sizeof(err));
        char j[400], ee[300];
        json_escape(err, ee, sizeof(ee));
        pthread_mutex_lock(&g_ls_pc_mtx);
        int live = g_ls_pc.waiting > 0;
        pthread_mutex_unlock(&g_ls_pc_mtx);
        if (id < 0) snprintf(j, sizeof(j), "{\"error\":\"%s\"}", ee);
        else snprintf(j, sizeof(j), "{\"ok\":true,\"id\":%d,\"state\":\"%s\",\"pc_connected\":%s}", id, id ? "waiting" : "ready",
                      live ? "true" : "false");
        http_json(sock, j);
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/pc/fetch_status?", 21) == 0) {
        const char *p = strstr(url, "id=");
        char j[600];
        ls_fetch_status_json(p ? atoi(p + 3) : 0, j, sizeof(j));
        http_json(sock, j);
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/pc/requests", 16) == 0 && (url[16] == 0 || url[16] == '?')) {
        if (!ls_pc_active_req(sock, url)) { http_json(sock, "{\"requests\":[]}"); return 1; }
        size_t cap = 2048, pos = 0;
        char *buf = malloc(cap);
        if (!buf) { http_json(sock, "{\"error\":\"Out of memory\"}"); return 1; }
        buf[0] = 0;
        ls_fetch_list_json(&buf, &cap, &pos);
        http_send(sock, "200 OK", "application/json", buf, (int)pos);
        free(buf);
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/pc/clients") == 0) {
        ls_config_t cfg;
        ls_load_config(&cfg);
        int64_t now = (int64_t)time(NULL);
        size_t cap = 2048, pos = 0;
        char *buf = malloc(cap);
        if (!buf) { http_json(sock, "{\"error\":\"Out of memory\"}"); return 1; }
        buf[0] = 0;
        ls_json_append(&buf, &cap, &pos, "{\"active\":\"%s\",\"clients\":[", cfg.pc_active);
        int first = 1;
        char listed[LS_PC_PEERS + 8][40];
        int nl = 0;
        pthread_mutex_lock(&g_ls_pc_mtx);
        for (int i = 0; i < LS_PC_PEERS; i++) {
            const ls_pc_peer_t *pe = &g_ls_peers[i];
            if (!pe->id[0]) continue;
            char en[140];
            json_escape(pe->name, en, sizeof(en));
            const char *st = ls_id_listed(cfg.pc_blocked, pe->id) ? "blocked" : strcmp(pe->id, cfg.pc_active) == 0 ? "active"
                           : strcmp(pe->id, g_ls_standin) == 0 ? "standin" : "standby";
            ls_json_append(&buf, &cap, &pos, "%s{\"id\":\"%s\",\"name\":\"%s\",\"kind\":\"%s\",\"version\":\"%s\",\"addr\":\"%s\","
                           "\"last_seen\":%lld,\"connected\":%s,\"state\":\"%s\"}", first ? "" : ",", pe->id, en, pe->kind, pe->version,
                           pe->addr, (long long)pe->last_seen, now - pe->last_seen < LS_PC_CONNECTED_S ? "true" : "false", st);
            snprintf(listed[nl++], 40, "%s", pe->id);
            first = 0;
        }
        pthread_mutex_unlock(&g_ls_pc_mtx);
        /* the active and blocked ones not seen since start */
        char all[260];
        snprintf(all, sizeof(all), "%s%s%s", cfg.pc_active, cfg.pc_active[0] && cfg.pc_blocked[0] ? "," : "", cfg.pc_blocked);
        for (char *p = all; *p && nl < LS_PC_PEERS + 8;) {
            char *e = strchr(p, ',');
            if (e) *e = 0;
            int seen = 0;
            for (int k = 0; k < nl; k++) seen |= strcmp(listed[k], p) == 0;
            if (*p && !seen) {
                ls_json_append(&buf, &cap, &pos, "%s{\"id\":\"%s\",\"name\":\"\",\"kind\":\"\",\"version\":\"\",\"addr\":\"\",\"last_seen\":0,"
                               "\"connected\":false,\"state\":\"%s\"}", first ? "" : ",", p,
                               ls_id_listed(cfg.pc_blocked, p) ? "blocked" : "active");
                snprintf(listed[nl++], 40, "%s", p);
                first = 0;
            }
            if (!e) break;
            p = e + 1;
        }
        ls_json_append(&buf, &cap, &pos, "]}");
        http_send(sock, "200 OK", "application/json", buf, (int)pos);
        free(buf);
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/pc/clients?", 16) == 0) {
        char id[40] = "", what[16] = "";
        ls_qparam(url, "id", id, sizeof(id));
        ls_qparam(url, "do", what, sizeof(what));
        ls_keep_word(id, ":._-");
        if (!id[0]) { http_json(sock, "{\"error\":\"no client\"}"); return 1; }
        pthread_mutex_lock(&g_ls_cfg_mtx);
        ls_config_t cfg;
        ls_load_config(&cfg);
        if (strcmp(what, "activate") == 0) {
            snprintf(cfg.pc_active, sizeof(cfg.pc_active), "%s", id);
            ls_id_list_remove(cfg.pc_blocked, id);
            int had = ls_queue_promote_standin(id);
            if (had) ls_event("PC_CLIENT_CHANGED", NULL, "%s already holds %d archive(s) collected as a stand-in", id, had);
            pthread_mutex_lock(&g_ls_pc_mtx);
            if (strcmp(g_ls_standin, id) == 0) g_ls_standin[0] = 0;
            pthread_mutex_unlock(&g_ls_pc_mtx);
        } else if (strcmp(what, "block") == 0) {
            if (!ls_id_listed(cfg.pc_blocked, id) && strlen(cfg.pc_blocked) + strlen(id) + 2 < sizeof(cfg.pc_blocked))
                snprintf(cfg.pc_blocked + strlen(cfg.pc_blocked), sizeof(cfg.pc_blocked) - strlen(cfg.pc_blocked), "%s%s",
                         cfg.pc_blocked[0] ? "," : "", id);
            if (strcmp(cfg.pc_active, id) == 0) cfg.pc_active[0] = 0;     /* the next client that connects takes over */
        } else if (strcmp(what, "unblock") == 0) {
            ls_id_list_remove(cfg.pc_blocked, id);
        } else if (strcmp(what, "forget") == 0) {
            ls_id_list_remove(cfg.pc_blocked, id);
            if (strcmp(cfg.pc_active, id) == 0) cfg.pc_active[0] = 0;
            pthread_mutex_lock(&g_ls_pc_mtx);
            for (int i = 0; i < LS_PC_PEERS; i++)
                if (strcmp(g_ls_peers[i].id, id) == 0) memset(&g_ls_peers[i], 0, sizeof(g_ls_peers[i]));
            pthread_mutex_unlock(&g_ls_pc_mtx);
        } else { pthread_mutex_unlock(&g_ls_cfg_mtx); http_json(sock, "{\"error\":\"unknown action\"}"); return 1; }
        int saved = ls_save_config(&cfg) == 0;
        pthread_mutex_unlock(&g_ls_cfg_mtx);
        if (!saved) { http_json(sock, "{\"error\":\"Could not write sync.json\"}"); return 1; }
        if (strcmp(what, "activate") == 0 || strcmp(what, "block") == 0 || strcmp(what, "forget") == 0) {
            pthread_mutex_lock(&g_ls_pc_mtx);
            int w = g_ls_pc.waiting;
            memset(&g_ls_pc, 0, sizeof(g_ls_pc));        /* the status now describes the new active client */
            g_ls_pc.waiting = w;
            pthread_mutex_unlock(&g_ls_pc_mtx);
        }
        ls_event("PC_CLIENT_CHANGED", NULL, "%s: %s", what, id);
        ls_wait_bump("config");
        http_json(sock, "{\"ok\":true}");
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/pc/status") == 0) {
        char j[1600];
        ls_pc_json(j, sizeof(j));
        http_json(sock, j);
        return 1;
    }
#ifndef LS_HOST_TEST
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/pc/shallot_sync.py") == 0) {
        ls_send_attachment(sock, "text/x-python", "shallot_sync.py", pc_shallot_sync_py, pc_shallot_sync_py_len);
        return 1;
    }
    /* files without an extension go out as octet-stream: as text/plain, browsers save "Dockerfile.txt" */
    {
        static const struct { const char *url, *name, *ctype; const unsigned char *data; const unsigned int *len; } files[] = {
            { "/api/pc/docker/Dockerfile", "Dockerfile", "application/octet-stream", pc_Dockerfile, &pc_Dockerfile_len },
            { "/api/pc/docker/docker-entrypoint.sh", "docker-entrypoint.sh", "text/x-shellscript", pc_docker_entrypoint_sh,
              &pc_docker_entrypoint_sh_len },
            { "/api/pc/vault/shallot_vault.py", "shallot_vault.py", "text/x-python", vault_shallot_vault_py, &vault_shallot_vault_py_len },
            { "/api/pc/vault/Dockerfile", "Dockerfile", "application/octet-stream", vault_Dockerfile, &vault_Dockerfile_len },
            { "/api/pc/vault/docker-compose.yml", "docker-compose.yml", "text/yaml", vault_docker_compose_yml,
              &vault_docker_compose_yml_len },
            { "/api/pc/vault/Caddyfile", "Caddyfile", "application/octet-stream", vault_Caddyfile, &vault_Caddyfile_len },
        };
        for (size_t i = 0; strcmp(method, "GET") == 0 && i < sizeof(files) / sizeof(files[0]); i++)
            if (strcmp(url, files[i].url) == 0) {
                ls_send_attachment(sock, files[i].ctype, files[i].name, files[i].data, *files[i].len);
                return 1;
            }
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/pc/docker/docker-compose.yml") == 0) {
        /* this PS5's address as the default, so no .env file is needed */
        char ip[48], r1[96], r2[120];
        ls_local_addr(sock, ip, sizeof(ip));
        ls_config_t cfg;
        ls_load_config(&cfg);
        snprintf(r1, sizeof(r1), "${SHALLOT_PS5_HOST:-%s}", ip);
        snprintf(r2, sizeof(r2), "${SHALLOT_CLIENT_KEY:-%s}", cfg.client_key);
        size_t n1 = 0, n2 = 0;
        char *a = ls_replace_once((const char *)pc_docker_compose_yml, pc_docker_compose_yml_len,
                                  "${SHALLOT_PS5_HOST:?set SHALLOT_PS5_HOST in .env}", r1, &n1);
        char *b = a ? ls_replace_once(a, n1, "${SHALLOT_CLIENT_KEY:-}", r2, &n2) : NULL;
        if (b) ls_send_attachment(sock, "text/yaml", "docker-compose.yml", b, n2);
        else http_json(sock, "{\"error\":\"Out of memory\"}");
        free(a);
        free(b);
        return 1;
    }
#endif
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/pc/config.json") == 0) {
        char ip[48], j[640], key[100] = "";
        ls_local_addr(sock, ip, sizeof(ip));
        ls_config_t cfg;
        ls_load_config(&cfg);
        if (cfg.client_key[0]) snprintf(key, sizeof(key), "  \"client_key\": \"%s\",\n", cfg.client_key);
        int n = snprintf(j, sizeof(j),
                         "{\n  \"ps5_host\": \"%s\",\n  \"port\": %d,\n  \"backup_dir\": \"ShallotBackups\",\n%s"
                         "  \"verify_downloads\": true\n}\n", ip, PORT, key);
        ls_send_attachment(sock, "application/json", "config.json", j, (size_t)n);
        return 1;
    }
    return 0;
}
