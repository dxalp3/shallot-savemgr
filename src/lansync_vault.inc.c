/*
 * LAN save sync — "VPS only": the PS5 talks to a Shallot vault directly.
 * Included from main.c after lansync_http.inc.c.
 *
 * Same format as the PC client's vault (pc/shallot_sync.py, class Vault), so
 * with the same key the PS5 and a PC write identical blocks and recipe
 * names, deduplicate against each other and can restore each other's
 * uploads:
 *   block   id = hex HMAC(id_key, bytes), blob = nonce | AES-256-GCM(enc_key, bytes, aad = id)
 *   archive meta "archive"/HMAC(id_key, "name:archive:<queue id>") = sealed recipe JSON
 *   snapshot meta "snapshot"/HMAC(id_key, "name:snapshot:<rel>") = sealed {"rel", "json"}
 * The PS5 has no TLS: requests go to the vault's signed-only plain port and
 * carry X-Shallot-Time / -Nonce / -Sig = HMAC(token, method, path, time,
 * nonce, sha256(body)); the token itself is never sent.
 *
 *   storage_mode "vps": every queued archive is uploaded (queue.vault_ack_at);
 *   the PS5 keeps archives until the vault has them, like the PC route.
 *   GET  /api/vault/status            state, pending, last ok, error
 *   GET  /api/vault/key[?reveal=1]    fingerprint (and the key, to copy to the PC)
 *   POST /api/vault/key  body = 64 hex (use the PC's key: both converge)
 *   POST /api/vault/test              signed ping
 *   POST /api/vault/sync              upload now
 */

#define LS_VAULT_KEY LS_BASE_DIR "/vault.key"
#define LS_VAULT_BATCH (8 * 1024 * 1024)

typedef struct { char host[128]; int port; char prefix[96]; } ls_vurl_t;

static pthread_mutex_t g_ls_vault_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct { char state[16]; int64_t pending, last_ok, last_try; char error[200]; } g_ls_vault = { "off", 0, 0, 0, "" };

static void ls_vault_set(const char *state, const char *error) {
    pthread_mutex_lock(&g_ls_vault_mtx);
    snprintf(g_ls_vault.state, sizeof(g_ls_vault.state), "%s", state);
    snprintf(g_ls_vault.error, sizeof(g_ls_vault.error), "%s", error ? error : "");
    g_ls_vault.last_try = (int64_t)time(NULL);
    if (strcmp(state, "ok") == 0 || strcmp(state, "pending") == 0) g_ls_vault.last_ok = g_ls_vault.last_try;
    pthread_mutex_unlock(&g_ls_vault_mtx);
}

static int ls_vurl_parse(const char *url, ls_vurl_t *u, char *err, size_t err_sz) {
    memset(u, 0, sizeof(*u));
    if (strncmp(url, "https://", 8) == 0) {
        snprintf(err, err_sz, "the PS5 cannot use https: use the vault's signed port, e.g. http://host:8091");
        return -1;
    }
    if (strncmp(url, "http://", 7) != 0) { snprintf(err, err_sz, "vault URL must start with http://"); return -1; }
    const char *h = url + 7, *slash = strchr(h, '/'), *colon = strchr(h, ':');
    size_t hl = slash ? (size_t)(slash - h) : strlen(h);
    if (colon && (!slash || colon < slash)) { hl = (size_t)(colon - h); u->port = atoi(colon + 1); }
    else u->port = 80;
    if (hl == 0 || hl >= sizeof(u->host) || u->port <= 0 || u->port > 65535) { snprintf(err, err_sz, "bad vault URL"); return -1; }
    memcpy(u->host, h, hl);
    if (slash) snprintf(u->prefix, sizeof(u->prefix), "%s", slash);
    size_t pl = strlen(u->prefix);
    while (pl && u->prefix[pl - 1] == '/') u->prefix[--pl] = 0;
    return 0;
}

/* One signed request. *resp (malloc'd) gets the body. Returns the HTTP status, or -1. */
/* The vault's clock minus ours (seconds): signatures carry a time the vault
 * accepts within 5 minutes, and a PS5 without internet can drift. Learned from
 * the X-Shallot-Server-Time header of every answer. */
static int64_t g_ls_vault_skew;

static int ls_vault_req_once(const ls_vurl_t *u, const char *token, const char *method, const char *path,
                             const void *body, size_t blen, char **resp, size_t *rlen, char *err, size_t err_sz) {
    *resp = NULL; *rlen = 0;
    char full[256], nonce_hex[33], ts[24], bh[65], sig[65];
    snprintf(full, sizeof(full), "%s%s", u->prefix, path);
    uint8_t nb[16], hb[32], sb[32];
    ls_random(nb, 16);
    ls_hex(nb, 16, nonce_hex);
    snprintf(ts, sizeof(ts), "%lld", (long long)(time(NULL) + g_ls_vault_skew));
    ls_sha256_t c;
    ls_sha256_init(&c);
    if (blen) ls_sha256_update(&c, body, blen);
    ls_sha256_final_bin(&c, hb);
    ls_hex(hb, 32, bh);
    char msg[512];
    int ml = snprintf(msg, sizeof(msg), "%s\n%s\n%s\n%s\n%s", method, full, ts, nonce_hex, bh);
    ls_hmac_sha256((const uint8_t *)token, strlen(token), msg, (size_t)ml, sb);
    ls_hex(sb, 32, sig);
    int fd = tcp_connect_host(u->host, u->port, err, err_sz);
    if (fd < 0) return -1;
    struct timeval tv = { 60, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    char hdr[1024];
    int hl = snprintf(hdr, sizeof(hdr),
                      "%s %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/octet-stream\r\nContent-Length: %zu\r\n"
                      "X-Shallot-Time: %s\r\nX-Shallot-Nonce: %s\r\nX-Shallot-Sig: %s\r\nConnection: close\r\n\r\n",
                      method, full, u->host, u->port, blen, ts, nonce_hex, sig);
    int ok = send(fd, hdr, (size_t)hl, 0) == hl;
    size_t off = 0;
    while (ok && off < blen) {
        ssize_t s = send(fd, (const char *)body + off, blen - off, 0);
        if (s <= 0) ok = 0; else off += (size_t)s;
    }
    if (!ok) { close(fd); snprintf(err, err_sz, "sending to the vault failed"); return -1; }
    size_t cap = 64 * 1024, got = 0;
    char *buf = malloc(cap + 1);
    while (buf) {
        if (got == cap) {
            if (cap > (size_t)48 * 1024 * 1024) break;
            char *nb2 = realloc(buf, cap * 2 + 1);
            if (!nb2) break;
            buf = nb2; cap *= 2;
        }
        ssize_t r = recv(fd, buf + got, cap - got, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!buf) { snprintf(err, err_sz, "out of memory"); return -1; }
    buf[got] = 0;
    int status = 0;
    if (sscanf(buf, "HTTP/%*s %d", &status) != 1) { free(buf); snprintf(err, err_sz, "no answer from the vault"); return -1; }
    char *b = strstr(buf, "\r\n\r\n");
    static const char time_hdr[] = "\r\nX-Shallot-Server-Time:";
    char *stime = strcasestr(buf, time_hdr);
    if (stime && (!b || stime < b)) {
        int64_t server = strtoll(stime + sizeof(time_hdr) - 1, NULL, 10), skew = server - (int64_t)time(NULL);
        if (server > 0) g_ls_vault_skew = (skew > 30 || skew < -30) ? skew : 0;
    }
    size_t start = b ? (size_t)(b + 4 - buf) : got;
    char *cl = strcasestr(buf, "\r\nContent-Length:");
    size_t clen = (cl && cl < b) ? strtoul(cl + 17, NULL, 10) : got - start;
    if (start + clen > got) { free(buf); snprintf(err, err_sz, "vault answer cut short"); return -1; }
    memmove(buf, buf + start, clen);
    buf[clen] = 0;
    *resp = buf; *rlen = clen;
    if (status == 401) snprintf(err, err_sz, "the vault refused the signature (wrong token?)");
    return status;
}

/* One retry when the vault refused us and turned out to have another clock. */
static int ls_vault_req(const ls_vurl_t *u, const char *token, const char *method, const char *path,
                        const void *body, size_t blen, char **resp, size_t *rlen, char *err, size_t err_sz) {
    int64_t before = g_ls_vault_skew;
    int st = ls_vault_req_once(u, token, method, path, body, blen, resp, rlen, err, err_sz);
    if (st == 401 && g_ls_vault_skew != before) {
        ls_event("VAULT_CLOCK", NULL, "the PS5's clock is %lld s off the vault's; requests are corrected", (long long)g_ls_vault_skew);
        free(*resp);
        err[0] = 0;
        st = ls_vault_req_once(u, token, method, path, body, blen, resp, rlen, err, err_sz);
    }
    return st;
}

/* ── key ── */
static int ls_vault_key(uint8_t secret[32], int create) {
    FILE *f = fopen(LS_VAULT_KEY, "rb");
    if (f) {
        char hex[80] = {0};
        size_t n = fread(hex, 1, sizeof(hex) - 1, f);
        fclose(f);
        hex[n] = 0;
        return (n >= 64 && ls_unhex(hex, secret, 32) == 0) ? 0 : -1;
    }
    if (!create) return -1;
    ls_random(secret, 32);
    char hex[66];
    ls_hex(secret, 32, hex);
    hex[64] = '\n'; hex[65] = 0;
    mkdir(LS_BASE_DIR, 0777);
    int fd = open(LS_VAULT_KEY, O_CREAT | O_WRONLY | O_EXCL, 0600);
    if (fd < 0) return -1;
    ls_write_all(fd, hex, 65);
    fsync(fd);
    close(fd);
    ls_event("VAULT_KEY_CREATED", NULL, "new vault key on the PS5: copy it (Backup & Sync → Off-site copy) to keep the VPS copy readable");
    return 0;
}

static void ls_vault_fingerprint(const uint8_t secret[32], char out[17]) {
    uint8_t d[32];
    ls_sha256_t c;
    ls_sha256_init(&c);
    ls_sha256_update(&c, secret, 32);
    ls_sha256_final_bin(&c, d);
    ls_hex(d, 8, out);
}

/* ── helpers: hex-id lists ── */
static int ls_hexid_cmp(const void *a, const void *b) { return memcmp(a, b, 64); }

/* Collects every 64-hex string inside quotes in s (sorted, unique). */
static int ls_parse_hex_list(const char *s, char (**out)[65]) {
    int cap = 256, n = 0;
    char (*v)[65] = malloc(sizeof(*v) * (size_t)cap);
    if (!v) return -1;
    for (const char *p = s; (p = strchr(p, '"')) != NULL; p++) {
        const char *q = p + 1;
        int k = 0;
        while (k < 64 && isxdigit((unsigned char)q[k])) k++;
        if (k == 64 && q[64] == '"') {
            if (n == cap) { cap *= 2; char (*nv)[65] = realloc(v, sizeof(*v) * (size_t)cap); if (!nv) { free(v); return -1; } v = nv; }
            memcpy(v[n], q, 64); v[n][64] = 0; n++;
            p = q + 64;
        }
    }
    qsort(v, (size_t)n, sizeof(*v), ls_hexid_cmp);
    *out = v;
    return n;
}

/* ── upload one archive ── */
static int ls_vault_push(const ls_vurl_t *u, const char *token, const ls_vkeys_t *k, const ls_queue_row_t *q,
                         char *iobuf, char *err, size_t err_sz) {
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", LS_QUEUE_DIR, q->filename);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { snprintf(err, err_sz, "%s: archive file missing", q->id); return -1; }
    int64_t first = ls_zip_image_offset(fd);
    int n = (int)(q->size / LS_BLOCK_SIZE) + 4, nb = 0, rc = -1;
    char (*ids)[65] = malloc(sizeof(*ids) * (size_t)n);
    char *json = NULL, *resp = NULL, (*miss)[65] = NULL;
    uint8_t *batch = NULL, *sealed = NULL;
    char *sent_flag = NULL;
    size_t rlen = 0;
    if (!ids) goto out;
    ls_sha256_t whole;
    ls_sha256_init(&whole);
    for (int64_t pos = 0; pos < q->size;) {
        int64_t len = (pos == 0 && first > 0) ? first : LS_BLOCK_SIZE;
        if (len > q->size - pos) len = q->size - pos;
        if (len > BUF_SIZE || pread(fd, iobuf, (size_t)len, (off_t)pos) != (ssize_t)len || nb >= n) { snprintf(err, err_sz, "cannot read %s", q->id); goto out; }
        ls_sha256_update(&whole, iobuf, (size_t)len);
        ls_vault_block_id(k, iobuf, (size_t)len, ids[nb++]);
        pos += len;
    }
    char sha[65];
    ls_sha256_final_hex(&whole, sha);
    if (strcmp(sha, q->sha256) != 0) { snprintf(err, err_sz, "%s does not match its SHA-256", q->id); goto out; }
    /* which blocks the vault lacks */
    size_t jcap = (size_t)nb * 67 + 1024;          /* ids + the recipe's header (key, sha256, size) */
    json = malloc(jcap);
    if (!json) goto out;
    size_t jl = (size_t)snprintf(json, jcap, "{\"ids\":[");
    for (int i = 0; i < nb; i++) jl += (size_t)snprintf(json + jl, jcap - jl, "%s\"%s\"", i ? "," : "", ids[i]);
    jl += (size_t)snprintf(json + jl, jcap - jl, "]}");
    int st = ls_vault_req(u, token, "POST", "/v1/blocks/missing", json, jl, &resp, &rlen, err, err_sz);
    if (st != 200) { if (st > 0 && !err[0]) snprintf(err, err_sz, "vault: HTTP %d", st); goto out; }
    int nm = ls_parse_hex_list(resp, &miss);
    free(resp); resp = NULL;
    if (nm < 0) goto out;
    /* upload them in batches: [32-byte id][4-byte length][nonce | ciphertext | tag] */
    batch = malloc(LS_VAULT_BATCH + LS_BLOCK_SIZE * 2 + 1024);
    sealed = malloc(BUF_SIZE + 64);
    sent_flag = calloc((size_t)nm + 1, 1);       /* separate flags: the sorted list must stay intact */
    if (!batch || !sealed || !sent_flag) goto out;
    size_t bl = 0;
    int64_t pos = 0;
    for (int i = 0; i < nb; i++) {
        int64_t len = (pos == 0 && first > 0) ? first : LS_BLOCK_SIZE;
        if (len > q->size - pos) len = q->size - pos;
        char (*hit)[65] = nm ? bsearch(ids[i], miss, (size_t)nm, sizeof(*miss), ls_hexid_cmp) : NULL;
        if (hit && !sent_flag[hit - miss]) {
            if (pread(fd, iobuf, (size_t)len, (off_t)pos) != (ssize_t)len) { snprintf(err, err_sz, "cannot read %s", q->id); goto out; }
            size_t sl = ls_vault_seal(k, ids[i], (const uint8_t *)iobuf, (size_t)len, sealed);
            if (bl + 36 + sl > (size_t)LS_VAULT_BATCH + LS_BLOCK_SIZE * 2) {
                int s2 = ls_vault_req(u, token, "POST", "/v1/blocks/batch", batch, bl, &resp, &rlen, err, err_sz);
                free(resp); resp = NULL;
                if (s2 != 200) { if (s2 > 0 && !err[0]) snprintf(err, err_sz, "vault: HTTP %d", s2); goto out; }
                bl = 0;
            }
            ls_unhex(ids[i], batch + bl, 32);
            batch[bl + 32] = (uint8_t)(sl >> 24); batch[bl + 33] = (uint8_t)(sl >> 16);
            batch[bl + 34] = (uint8_t)(sl >> 8); batch[bl + 35] = (uint8_t)sl;
            memcpy(batch + bl + 36, sealed, sl);
            bl += 36 + sl;
            sent_flag[hit - miss] = 1;          /* sent once, even if repeated in this archive */
        }
        pos += len;
    }
    if (bl) {
        int s2 = ls_vault_req(u, token, "POST", "/v1/blocks/batch", batch, bl, &resp, &rlen, err, err_sz);
        free(resp); resp = NULL;
        if (s2 != 200) { if (s2 > 0 && !err[0]) snprintf(err, err_sz, "vault: HTTP %d", s2); goto out; }
    }
    /* recipe (the PC client's format) */
    char et[64], es[520], eu[40];
    json_escape(q->title_id, et, sizeof(et));
    json_escape(q->save_name, es, sizeof(es));
    json_escape(q->user_id, eu, sizeof(eu));
    jl = (size_t)snprintf(json, jcap, "{\"v\":1,\"key\":\"%s\",\"sha256\":\"%s\",\"size\":%lld,\"ids\":[", q->id, q->sha256, (long long)q->size);
    for (int i = 0; i < nb; i++) jl += (size_t)snprintf(json + jl, jcap - jl, "%s\"%s\"", i ? "," : "", ids[i]);
    char tail[1400];
    int tl = snprintf(tail, sizeof(tail), "],\"entry\":{\"title_id\":\"%s\",\"save_name\":\"%s\",\"user_id\":\"%s\",\"created_at\":%lld,"
                      "\"contents\":null,\"path\":\"%s/%s/%s.zip\",\"origin\":\"ps5\"}}", et, es, eu, (long long)q->created_at, et, es, q->id);
    if (jl + (size_t)tl + 1 > jcap) { char *nj = realloc(json, jl + (size_t)tl + 1); if (!nj) goto out; json = nj; jcap = jl + (size_t)tl + 1; }
    memcpy(json + jl, tail, (size_t)tl + 1);
    jl += (size_t)tl;
    char name[65], aad[80], mpath[160];
    char nk[260];
    snprintf(nk, sizeof(nk), "archive:%s", q->id);
    ls_vault_name(k, nk, name);
    snprintf(aad, sizeof(aad), "archive:%s", name);
    uint8_t *mblob = malloc(jl + 28);
    if (!mblob) goto out;
    size_t ml = ls_vault_seal(k, aad, (const uint8_t *)json, jl, mblob);
    snprintf(mpath, sizeof(mpath), "/v1/meta/archive/%s", name);
    st = ls_vault_req(u, token, "PUT", mpath, mblob, ml, &resp, &rlen, err, err_sz);
    free(mblob);
    if (st != 200) { if (st > 0 && !err[0]) snprintf(err, err_sz, "vault: HTTP %d", st); goto out; }
    free(resp); resp = NULL;
    {   /* the archive's block ids in the clear (keyed hashes: they reveal nothing) for the vault's clean-up */
        uint8_t *refs = malloc((size_t)nb * 32 + 1);
        if (refs) {
            for (int i = 0; i < nb; i++) ls_unhex(ids[i], refs + (size_t)i * 32, 32);
            snprintf(mpath, sizeof(mpath), "/v1/meta/refs/%s", name);
            char e3[120] = "";
            ls_vault_req(u, token, "PUT", mpath, refs, (size_t)nb * 32, &resp, &rlen, e3, sizeof(e3));   /* older vaults: ignored */
            free(refs);
            free(resp); resp = NULL;
        }
    }
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *up = NULL;
    if (sqlite3_prepare_v2(g_ls_db, "UPDATE queue SET vault_ack_at = ? WHERE id = ?", -1, &up, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(up, 1, (sqlite3_int64)time(NULL));
        ls_bind_text(up, 2, q->id);
        sqlite3_step(up);
    }
    sqlite3_finalize(up);
    pthread_mutex_unlock(&g_ls_mtx);
    rc = 0;
out:
    close(fd);
    free(ids); free(json); free(resp); free(miss); free(batch); free(sealed); free(sent_flag);
    return rc;
}

/* Restore points as the PC stores them: <TITLE>/_snapshots/<id>.json */
static int ls_vault_push_snapshot(const ls_vurl_t *u, const char *token, const ls_vkeys_t *k, int64_t snap_id,
                                  char *err, size_t err_sz) {
    size_t cap = 16 * 1024, pos = 0;
    char *sj = malloc(cap);
    if (!sj) return -1;
    sj[0] = 0;
    ls_snapshot_json(snap_id, &sj, &cap, &pos);
    char title[32] = {0}, user[16], kind[16], obj[256];
    if (!ls_snapshot_head(snap_id, user, title, kind, obj)) { free(sj); return 0; }
    char rel[96];
    snprintf(rel, sizeof(rel), "%s/_snapshots/%08lld.json", title, (long long)snap_id);
    size_t ecap = pos * 2 + 256;
    char *wrap = malloc(ecap + 64);
    if (!wrap) { free(sj); return -1; }
    size_t wl = (size_t)snprintf(wrap, ecap, "{\"rel\":\"%s\",\"json\":\"", rel);
    json_escape(sj, wrap + wl, ecap - wl);
    wl = strlen(wrap);
    wl += (size_t)snprintf(wrap + wl, 64, "\"}");
    free(sj);
    char nk[128], name[65], aad[80], mpath[160];
    snprintf(nk, sizeof(nk), "snapshot:%s", rel);
    ls_vault_name(k, nk, name);
    snprintf(aad, sizeof(aad), "snapshot:%s", name);
    uint8_t *blob = malloc(wl + 28);
    if (!blob) { free(wrap); return -1; }
    size_t bl = ls_vault_seal(k, aad, (const uint8_t *)wrap, wl, blob);
    free(wrap);
    snprintf(mpath, sizeof(mpath), "/v1/meta/snapshot/%s", name);
    char *resp = NULL;
    size_t rlen = 0;
    int st = ls_vault_req(u, token, "PUT", mpath, blob, bl, &resp, &rlen, err, err_sz);
    free(blob); free(resp);
    if (st != 200) { if (st > 0 && !err[0]) snprintf(err, err_sz, "vault: HTTP %d", st); return -1; }
    return 1;
}

static void ls_vault_mark_gone(const char *sql, const char *key) {
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *st = NULL;
    if (g_ls_db && sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK) {
        ls_bind_text(st, 1, key);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
}

/*
 * The vault follows the history schedule too (keep everything for the first
 * days, then one per day, then one per week; Backup & Sync settings), unless
 * vault_keep_all. Restore points are thinned first; an archive stays while a
 * remaining restore point needs it. Then the vault deletes blocks no archive
 * uses any more (POST /v1/gc). At most daily. Returns archives dropped.
 */
static int ls_vault_thin(const ls_vurl_t *u, const char *token, const ls_vkeys_t *k, int64_t now, char *err, size_t err_sz) {
    ls_config_t cfg;
    ls_load_config(&cfg);
    if (cfg.vault_keep_all || now - ls_meta_get_int("last_vault_thin", 0) < 86400) return 0;
    int64_t upto = ls_meta_get_int("vault_snap_upto", 0);
    int ns = 0, cap = 0, dropped_snaps = 0, dropped = 0;
    ls_thin_item_t *sn = NULL;
    char (*stitle)[32] = NULL;
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_ls_db, "SELECT snap_id, created_at, user_id, title_id, kind, object_id FROM snapshots"
                                    " WHERE snap_id <= ? AND vault_gone = 0", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)upto);
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (ns == cap) {
                cap = cap ? cap * 2 : 256;
                ls_thin_item_t *nv = realloc(sn, sizeof(*sn) * (size_t)cap);
                char (*nt)[32] = realloc(stitle, sizeof(*stitle) * (size_t)cap);
                if (nv) sn = nv;
                if (nt) stitle = nt;
                if (!nv || !nt) break;
            }
            ls_thin_item_t *x = &sn[ns];
            memset(x, 0, sizeof(*x));
            snprintf(x->key, sizeof(x->key), "%lld", (long long)sqlite3_column_int64(st, 0));
            x->ts = sqlite3_column_int64(st, 1);
            snprintf(x->group, sizeof(x->group), "%s|%s|%s|%s", (const char *)sqlite3_column_text(st, 2) ?: "",
                     (const char *)sqlite3_column_text(st, 3) ?: "", (const char *)sqlite3_column_text(st, 4) ?: "",
                     (const char *)sqlite3_column_text(st, 5) ?: "");
            snprintf(stitle[ns], sizeof(stitle[ns]), "%s", (const char *)sqlite3_column_text(st, 3) ?: "");
            ns++;
        }
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    if (ns) ls_thin_mark(sn, ns, now, &cfg);
    char *resp = NULL;
    size_t rlen = 0;
    int failed = 0;
    for (int i = 0; i < ns && !failed; i++) {
        if (sn[i].keep) continue;
        char rel[96], nk[128], name[65], mpath[160];
        snprintf(rel, sizeof(rel), "%s/_snapshots/%08lld.json", stitle[i], atoll(sn[i].key));
        snprintf(nk, sizeof(nk), "snapshot:%s", rel);
        ls_vault_name(k, nk, name);
        snprintf(mpath, sizeof(mpath), "/v1/meta/snapshot/%s", name);
        int s = ls_vault_req(u, token, "DELETE", mpath, NULL, 0, &resp, &rlen, err, err_sz);
        free(resp); resp = NULL;
        if (s != 200) { failed = 1; break; }
        ls_vault_mark_gone("UPDATE snapshots SET vault_gone = 1 WHERE snap_id = CAST(? AS INTEGER)", sn[i].key);
        dropped_snaps++;
    }
    free(sn); free(stitle);
    /* archives in the vault */
    int na = 0, acap = 0;
    ls_thin_item_t *ar = NULL;
    pthread_mutex_lock(&g_ls_mtx);
    if (!failed && sqlite3_prepare_v2(g_ls_db, "SELECT id, created_at, user_id, title_id, save_name FROM queue"
                                               " WHERE vault_ack_at > 0 AND vault_gone = 0", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (na == acap) { acap = acap ? acap * 2 : 256; ls_thin_item_t *nv = realloc(ar, sizeof(*ar) * (size_t)acap); if (!nv) break; ar = nv; }
            ls_thin_item_t *x = &ar[na++];
            memset(x, 0, sizeof(*x));
            snprintf(x->key, sizeof(x->key), "%s", (const char *)sqlite3_column_text(st, 0) ?: "");
            x->ts = sqlite3_column_int64(st, 1);
            snprintf(x->group, sizeof(x->group), "%s|%s|%s", (const char *)sqlite3_column_text(st, 2) ?: "",
                     (const char *)sqlite3_column_text(st, 3) ?: "", (const char *)sqlite3_column_text(st, 4) ?: "");
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    if (na) ls_thin_mark(ar, na, now, &cfg);
    for (int i = 0; i < na && !failed; i++) {
        if (ar[i].keep) continue;
        pthread_mutex_lock(&g_ls_mtx);
        int64_t needed = ls_db_int_locked("SELECT COUNT(*) FROM snapshot_files f JOIN snapshots s ON s.snap_id = f.snap_id"
                                          " JOIN queue q ON q.gen_id = f.gen_id WHERE q.id = ? AND q.gen_id > 0 AND s.vault_gone = 0"
                                          " AND ? = ? AND 0 * ? = 0", ar[i].key, "x", "x", 0);
        pthread_mutex_unlock(&g_ls_mtx);
        if (needed > 0) continue;
        char nk[260], name[65], mpath[160];
        snprintf(nk, sizeof(nk), "archive:%s", ar[i].key);
        ls_vault_name(k, nk, name);
        snprintf(mpath, sizeof(mpath), "/v1/meta/archive/%s", name);
        int s = ls_vault_req(u, token, "DELETE", mpath, NULL, 0, &resp, &rlen, err, err_sz);
        free(resp); resp = NULL;
        if (s != 200) { failed = 1; break; }
        snprintf(mpath, sizeof(mpath), "/v1/meta/refs/%s", name);
        char e3[120] = "";
        ls_vault_req(u, token, "DELETE", mpath, NULL, 0, &resp, &rlen, e3, sizeof(e3));
        free(resp); resp = NULL;
        ls_vault_mark_gone("UPDATE queue SET vault_gone = 1 WHERE id = ?", ar[i].key);
        dropped++;
    }
    free(ar);
    if (failed) return -1;
    int64_t freed = 0;
    if (dropped || now - ls_meta_get_int("last_vault_gc", 0) > 7 * 86400) {
        char e3[200] = "";
        int s = ls_vault_req(u, token, "POST", "/v1/gc", "{}", 2, &resp, &rlen, e3, sizeof(e3));
        if (s == 200 && resp) {
            const char *p = strstr(resp, "\"deleted\":");
            if (p) freed = strtoll(p + 10, NULL, 10);
            ls_meta_set_int("last_vault_gc", now);
        }
        free(resp); resp = NULL;
    }
    ls_meta_set_int("last_vault_thin", now);
    if (dropped || dropped_snaps || freed)
        ls_event("VAULT_THINNED", NULL, "%d old version(s) and %d restore point(s) thinned out of the vault; %lld unused block(s) deleted",
                 dropped, dropped_snaps, (long long)freed);
    return dropped;
}

static int64_t ls_vault_pending_count(void) {
    pthread_mutex_lock(&g_ls_mtx);
    int64_t n = ls_db_int_locked("SELECT COUNT(*) FROM queue WHERE state IN ('ready','packed') AND vault_ack_at = 0"
                                 " AND ? = ? AND ? IS NOT NULL AND 0 * ? = 0", "x", "x", "x", 0);
    pthread_mutex_unlock(&g_ls_mtx);
    return n;
}

/* Worker job: upload what the vault does not have yet (time-boxed). */
static void ls_vault_job(void) {
    ls_config_t cfg;
    ls_load_config(&cfg);
    if (!cfg.storage_vps) { ls_vault_set("off", NULL); return; }
    char err[220] = "";
    ls_vurl_t u;
    if (!cfg.vault_url[0] || !cfg.vault_token[0]) { ls_vault_set("error", "vault URL and token are not set"); return; }
    if (ls_vurl_parse(cfg.vault_url, &u, err, sizeof(err)) < 0) { ls_vault_set("error", err); return; }
    uint8_t secret[32];
    if (ls_vault_key(secret, 1) < 0) { ls_vault_set("error", "cannot read or create the vault key"); return; }
    ls_vkeys_t k;
    ls_vkeys_init(&k, secret);
    char *iobuf = malloc(BUF_SIZE);
    if (!iobuf) return;
    int64_t t0 = (int64_t)time(NULL);
    int sent = 0, failed = 0;
    for (;;) {
        if ((int64_t)time(NULL) - t0 > 60 || ls_power_down()) break;
        char id[200] = "";
        pthread_mutex_lock(&g_ls_mtx);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "SELECT id FROM queue WHERE state IN ('ready','packed') AND vault_ack_at = 0"
                                        " ORDER BY created_at LIMIT 1", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
            ls_col_text(st, 0, id, sizeof(id));
        sqlite3_finalize(st);
        pthread_mutex_unlock(&g_ls_mtx);
        if (!id[0]) break;
        char e2[200];
        if (ls_archive_ensure_ready(id, e2, sizeof(e2)) < 0) { snprintf(err, sizeof(err), "%s", e2); failed++; break; }
        ls_queue_row_t q;
        if (!ls_queue_get(id, &q)) break;
        err[0] = 0;
        if (ls_vault_push(&u, cfg.vault_token, &k, &q, iobuf, err, sizeof(err)) < 0) { failed++; break; }
        sent++;
    }
    int snaps = 0;
    if (!failed) {
        int64_t upto = ls_meta_get_int("vault_snap_upto", 0);
        for (;;) {
            int64_t sid = 0;
            pthread_mutex_lock(&g_ls_mtx);
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(g_ls_db, "SELECT snap_id FROM snapshots WHERE snap_id > ? ORDER BY snap_id LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, (sqlite3_int64)upto);
                if (sqlite3_step(st) == SQLITE_ROW) sid = sqlite3_column_int64(st, 0);
            }
            sqlite3_finalize(st);
            pthread_mutex_unlock(&g_ls_mtx);
            if (!sid || (int64_t)time(NULL) - t0 > 90) break;
            err[0] = 0;
            if (ls_vault_push_snapshot(&u, cfg.vault_token, &k, sid, err, sizeof(err)) < 0) { failed++; break; }
            upto = sid;
            ls_meta_set_int("vault_snap_upto", upto);
            snaps++;
        }
    }
    free(iobuf);
    int64_t left = ls_vault_pending_count();
    if (!failed && !left) {
        err[0] = 0;
        if (ls_vault_thin(&u, cfg.vault_token, &k, (int64_t)time(NULL), err, sizeof(err)) < 0) failed++;
    }
    pthread_mutex_lock(&g_ls_vault_mtx);
    g_ls_vault.pending = left;
    pthread_mutex_unlock(&g_ls_vault_mtx);
    if (failed) {
        ls_vault_set("error", err);
        ls_event("VAULT_UPLOAD_FAILED", NULL, "%s (retrying later)", err);
        ls_worker_enqueue(LS_JOB_VAULT, "", 0, 300);
    } else {
        ls_vault_set(left ? "pending" : "ok", NULL);
        if (left) ls_worker_enqueue(LS_JOB_VAULT, "", 0, 2);
    }
    if (sent || snaps) ls_event("VAULT_UPLOADED", NULL, "%d archive(s), %d restore point(s) sent to the vault; %lld waiting", sent, snaps, (long long)left);
}

/* ── fetch back (Get from VPS) ── */
typedef struct { uint8_t id[32]; const uint8_t *blob; uint32_t len; } ls_vrec_t;
static int ls_vrec_cmp(const void *a, const void *b) { return memcmp(a, b, 32); }

/* Rebuilds one archive from the vault into dst; checks every block and the whole SHA-256. */
static int ls_vault_fetch_archive(const ls_vurl_t *u, const char *token, const ls_vkeys_t *k, const char *qid,
                                  const char *expect_sha, const char *dst, char *err, size_t err_sz) {
    char nk[260], name[65], aad[80], mpath[160];
    snprintf(nk, sizeof(nk), "archive:%s", qid);
    ls_vault_name(k, nk, name);
    snprintf(aad, sizeof(aad), "archive:%s", name);
    snprintf(mpath, sizeof(mpath), "/v1/meta/archive/%s", name);
    char *resp = NULL;
    size_t rlen = 0;
    int st = ls_vault_req(u, token, "GET", mpath, NULL, 0, &resp, &rlen, err, err_sz);
    if (st != 200) { free(resp); if (st == 404) snprintf(err, err_sz, "the vault has no copy of %s", qid); else if (st > 0 && !err[0]) snprintf(err, err_sz, "vault: HTTP %d", st); return -1; }
    char *recipe = malloc(rlen + 1);
    int64_t m = recipe ? ls_vault_open(k, aad, (const uint8_t *)resp, rlen, (uint8_t *)recipe) : -1;
    free(resp);
    if (m < 0) { free(recipe); snprintf(err, err_sz, "vault recipe failed authentication (wrong key?)"); return -1; }
    recipe[m] = 0;
    char sha[65] = "";
    const char *sp = strstr(recipe, "\"sha256\":\"");
    if (sp) { memcpy(sha, sp + 10, 64); sha[64] = 0; }
    if (strcmp(sha, expect_sha) != 0) { free(recipe); snprintf(err, err_sz, "the vault's copy is a different archive"); return -1; }
    const char *ip = strstr(recipe, "\"ids\":[");
    const char *ie = ip ? strchr(ip, ']') : NULL;
    if (!ip || !ie) { free(recipe); snprintf(err, err_sz, "bad recipe"); return -1; }
    int nids = (int)((ie - ip) / 67) + 2, n = 0;
    char (*ids)[65] = malloc(sizeof(*ids) * (size_t)nids);
    for (const char *p = ip + 7; ids && p < ie && n < nids;) {
        const char *q = strchr(p, '"');
        if (!q || q >= ie) break;
        memcpy(ids[n], q + 1, 64); ids[n][64] = 0; n++;
        p = q + 66;
    }
    free(recipe);
    if (!ids) { snprintf(err, err_sz, "out of memory"); return -1; }
    int out = open(dst, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    uint8_t *plain = malloc(BUF_SIZE);
    char *req = malloc((size_t)2000 * 67 + 32);
    int ok = out >= 0 && plain && req;
    ls_sha256_t whole;
    ls_sha256_init(&whole);
    for (int i = 0; ok && i < n; i += 2000) {
        int cnt = n - i < 2000 ? n - i : 2000;
        size_t jl = (size_t)sprintf(req, "{\"ids\":[");
        for (int j = 0; j < cnt; j++) jl += (size_t)sprintf(req + jl, "%s\"%s\"", j ? "," : "", ids[i + j]);
        jl += (size_t)sprintf(req + jl, "]}");
        st = ls_vault_req(u, token, "POST", "/v1/blocks/fetch", req, jl, &resp, &rlen, err, err_sz);
        if (st != 200) { if (st > 0 && !err[0]) snprintf(err, err_sz, "vault: HTTP %d", st); ok = 0; free(resp); break; }
        int nr = 0, rc2 = (int)(rlen / 36) + 1;
        ls_vrec_t *recs = malloc(sizeof(*recs) * (size_t)rc2);
        for (size_t p = 0; recs && p + 36 <= rlen && nr < rc2;) {
            uint32_t l = ((uint32_t)(uint8_t)resp[p + 32] << 24) | ((uint32_t)(uint8_t)resp[p + 33] << 16) |
                         ((uint32_t)(uint8_t)resp[p + 34] << 8) | (uint32_t)(uint8_t)resp[p + 35];
            if (p + 36 + l > rlen) break;
            memcpy(recs[nr].id, resp + p, 32);
            recs[nr].blob = (const uint8_t *)resp + p + 36;
            recs[nr].len = l;
            nr++;
            p += 36 + l;
        }
        if (recs) qsort(recs, (size_t)nr, sizeof(*recs), ls_vrec_cmp);
        for (int j = 0; ok && j < cnt; j++) {
            ls_vrec_t key;
            ls_unhex(ids[i + j], key.id, 32);
            ls_vrec_t *r = recs ? bsearch(&key, recs, (size_t)nr, sizeof(*recs), ls_vrec_cmp) : NULL;
            if (!r || r->len > BUF_SIZE + 28) { snprintf(err, err_sz, "block %.12s is missing on the vault", ids[i + j]); ok = 0; break; }
            int64_t pl = ls_vault_open(k, ids[i + j], r->blob, r->len, plain);
            char check[65];
            if (pl >= 0) ls_vault_block_id(k, plain, (size_t)pl, check);
            if (pl < 0 || strcmp(check, ids[i + j]) != 0) { snprintf(err, err_sz, "vault block failed authentication"); ok = 0; break; }
            ls_sha256_update(&whole, plain, (size_t)pl);
            if (ls_write_all(out, (const char *)plain, (size_t)pl) < 0) { ok = 0; break; }
        }
        free(recs);
        free(resp);
        resp = NULL;
    }
    if (out >= 0) { if (ok && fsync(out) < 0) ok = 0; close(out); }
    free(plain); free(req); free(ids);
    char got[65];
    ls_sha256_final_hex(&whole, got);
    if (ok && strcmp(got, expect_sha) != 0) { snprintf(err, err_sz, "rebuilt archive does not match its SHA-256"); ok = 0; }
    if (!ok) unlink(dst);
    return ok ? 0 : -1;
}

/* Background: the archives of a fetch request, straight from the vault. */
static void *ls_vault_fetch_thread(void *arg) {
    int fid = (int)(intptr_t)arg;
    char qids[LS_FETCH_FILES][200];
    int n = 0;
    pthread_mutex_lock(&g_ls_fetch_mtx);
    for (int i = 0; i < LS_FETCH_MAX; i++)
        if (g_ls_fetch[i].id == fid)
            for (int k2 = 0; k2 < g_ls_fetch[i].n; k2++) if (!g_ls_fetch[i].done[k2]) snprintf(qids[n++], sizeof(qids[0]), "%s", g_ls_fetch[i].qid[k2]);
    pthread_mutex_unlock(&g_ls_fetch_mtx);
    ls_config_t cfg;
    ls_load_config(&cfg);
    ls_vurl_t u;
    char err[220] = "";
    uint8_t secret[32];
    if (ls_vurl_parse(cfg.vault_url, &u, err, sizeof(err)) < 0 || ls_vault_key(secret, 0) < 0) {
        for (int i = 0; i < n; i++) ls_fetch_failed(qids[i], err[0] ? err : "no vault key on this PS5");
        return NULL;
    }
    ls_vkeys_t k;
    ls_vkeys_init(&k, secret);
    for (int i = 0; i < n; i++) {
        ls_queue_row_t q;
        if (!ls_queue_get(qids[i], &q)) continue;
        if (strcmp(q.state, "ready") == 0 || strcmp(q.state, "packed") == 0) { ls_fetch_arrived(qids[i]); continue; }
        char final_path[MAX_PATH_LEN], part[MAX_PATH_LEN];
        snprintf(final_path, sizeof(final_path), "%s/%s", LS_QUEUE_DIR, q.filename);
        snprintf(part, sizeof(part), "%s.vault", final_path);
        err[0] = 0;
        if (ls_vault_fetch_archive(&u, cfg.vault_token, &k, q.id, q.sha256, part, err, sizeof(err)) < 0 || rename(part, final_path) < 0) {
            unlink(part);
            ls_fetch_failed(qids[i], err[0] ? err : "could not store the archive");
            ls_event("VAULT_FETCH_FAILED", q.title_id, "%s: %s", q.id, err);
            continue;
        }
        pthread_mutex_lock(&g_ls_mtx);
        sqlite3_stmt *up = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "UPDATE queue SET state = 'ready', vault_ack_at = ? WHERE id = ?", -1, &up, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(up, 1, (sqlite3_int64)time(NULL));
            ls_bind_text(up, 2, q.id);
            sqlite3_step(up);
        }
        sqlite3_finalize(up);
        pthread_mutex_unlock(&g_ls_mtx);
        ls_event("ARCHIVE_FROM_VAULT", q.title_id, "id=%s save=%s back on the PS5 (every block and the sha256 verified)", q.id, q.save_name);
        ls_fetch_arrived(qids[i]);
    }
    return NULL;
}

static void ls_vault_fetch_start(int fetch_id) {
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    if (pthread_create(&tid, &attr, ls_vault_fetch_thread, (void *)(intptr_t)fetch_id) == 0) pthread_detach(tid);
    pthread_attr_destroy(&attr);
}

/* The worker and the PC link reach this file through pointers (main() calls this first). */
static int ls_vault_key_hex(char hex[65]) {
    uint8_t secret[32];
    if (ls_vault_key(secret, 1) < 0) return -1;
    ls_hex(secret, 32, hex);
    return 0;
}

/*
 * Problems that deserve a PS5 notification, not just a line in the web UI
 * (checked at most hourly, each repeated at most daily):
 *   - the PS5 cannot reach its vault for over an hour;
 *   - backups have waited for the PC for over 3 days;
 *   - the PS5's backup queue is over its size limit.
 */
static void ls_health(int64_t now) {
    static int64_t last_check;
    if (now - last_check < 3600) return;
    last_check = now;
    ls_config_t cfg;
    ls_load_config(&cfg);
    if (cfg.storage_vps) {
        pthread_mutex_lock(&g_ls_vault_mtx);
        int bad = strcmp(g_ls_vault.state, "error") == 0;
        int64_t ok_at = g_ls_vault.last_ok;
        char e[200];
        snprintf(e, sizeof(e), "%s", g_ls_vault.error);
        pthread_mutex_unlock(&g_ls_vault_mtx);
        int64_t since = ok_at ? ok_at : ls_meta_get_int("vault_err_first", 0);
        if (bad && !since) ls_meta_set_int("vault_err_first", now);
        if (!bad) ls_meta_set_int("vault_err_first", 0);
        if (bad && since && now - since > 3600 && now - ls_meta_get_int("notified_vault", 0) > 86400) {
            notify("Shallot SaveMgr: the VPS copy has been failing for %lld h: %s", (long long)((now - since) / 3600), e);
            ls_meta_set_int("notified_vault", now);
        }
    }
    if (cfg.storage_pc) {
        int64_t oldest = 0;
        pthread_mutex_lock(&g_ls_mtx);
        if (g_ls_db)
            oldest = ls_db_int_locked("SELECT COALESCE(MIN(created_at), 0) FROM queue WHERE state IN ('ready','packed') AND lan_ack_at = 0"
                                      " AND ? = ? AND ? IS NOT NULL AND 0 * ? = 0", "x", "x", "x", 0);
        pthread_mutex_unlock(&g_ls_mtx);
        if (oldest && now - oldest > 3 * 86400 && now - ls_meta_get_int("notified_pc", 0) > 86400) {
            notify("Shallot SaveMgr: backups have waited %lld days for the PC; start its sync client", (long long)((now - oldest) / 86400));
            ls_meta_set_int("notified_pc", now);
        }
    }
    if (g_ls_queue_constrained && now - ls_meta_get_int("notified_queue", 0) > 86400) {
        notify("Shallot SaveMgr: backups waiting on the PS5 are over the size limit (Backup & Sync settings)");
        ls_meta_set_int("notified_queue", now);
    }
}

static void ls_vault_register(void) {
    g_ls_health = ls_health;
    g_ls_vault_job = ls_vault_job;
    g_ls_vault_fetch_start = ls_vault_fetch_start;
    g_ls_vault_key_hex = ls_vault_key_hex;
}

static void ls_vault_status_json(char *out, size_t out_sz) {
    ls_config_t cfg;
    ls_load_config(&cfg);
    uint8_t secret[32];
    char fp[17] = "";
    if (ls_vault_key(secret, 0) == 0) ls_vault_fingerprint(secret, fp);
    pthread_mutex_lock(&g_ls_vault_mtx);
    char ee[420];
    json_escape(g_ls_vault.error, ee, sizeof(ee));
    snprintf(out, out_sz, "{\"enabled\":%s,\"route\":\"%s\",\"configured\":%s,\"state\":\"%s\",\"pending\":%lld,\"last_ok\":%lld,\"error\":\"%s\",\"key\":\"%s\"}",
             cfg.storage_vps ? "true" : "false", cfg.storage_vps ? "ps5" : cfg.vps_via_pc ? "pc" : "off",
             cfg.vault_url[0] && cfg.vault_token[0] ? "true" : "false", cfg.storage_vps ? g_ls_vault.state : "off", (long long)g_ls_vault.pending,
             (long long)g_ls_vault.last_ok, ee, fp);
    pthread_mutex_unlock(&g_ls_vault_mtx);
}

static int ls_vault_route(int sock, const char *method, const char *url, char *req, int n) {
    if (strncmp(url, "/api/vault/", 11) != 0) return 0;
    char j[700];
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/vault/status") == 0) {
        ls_vault_status_json(j, sizeof(j));
        http_json(sock, j);
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/vault/key", 14) == 0) {
        uint8_t secret[32];
        if (ls_vault_key(secret, strstr(url, "create=1") != NULL) < 0) { http_json(sock, "{\"key\":null,\"fingerprint\":\"\"}"); return 1; }
        char hex[65], fp[17];
        ls_hex(secret, 32, hex);
        ls_vault_fingerprint(secret, fp);
        if (strstr(url, "reveal=1")) snprintf(j, sizeof(j), "{\"fingerprint\":\"%s\",\"key\":\"%s\"}", fp, hex);
        else snprintf(j, sizeof(j), "{\"fingerprint\":\"%s\"}", fp);
        http_json(sock, j);
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/vault/key") == 0) {
        char *iobuf = malloc(BUF_SIZE), *body = NULL, err[128] = "";
        size_t blen = 0;
        if (!iobuf || read_request_body(sock, req, n, iobuf, &body, &blen, err, sizeof(err)) < 0) {
            free(iobuf); free(body); http_json(sock, "{\"error\":\"no key in the request\"}"); return 1;
        }
        char hex[80] = "";
        size_t hl = 0;
        for (size_t i = 0; i < blen && hl < 64; i++) if (isxdigit((unsigned char)body[i])) hex[hl++] = (char)tolower((unsigned char)body[i]);
        hex[hl] = 0;
        free(iobuf); free(body);
        uint8_t secret[32];
        if (hl != 64 || ls_unhex(hex, secret, 32) < 0) { http_json(sock, "{\"error\":\"a vault key is 64 hex characters\"}"); return 1; }
        char line[66];
        memcpy(line, hex, 64); line[64] = '\n'; line[65] = 0;
        char tmp[MAX_PATH_LEN];
        snprintf(tmp, sizeof(tmp), "%s.tmp", LS_VAULT_KEY);
        int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        int ok = fd >= 0 && ls_write_all(fd, line, 65) == 0 && fsync(fd) == 0;
        if (fd >= 0) close(fd);
        if (!ok || rename(tmp, LS_VAULT_KEY) < 0) { unlink(tmp); http_json(sock, "{\"error\":\"cannot store the key\"}"); return 1; }
        char fp[17];
        ls_vault_fingerprint(secret, fp);
        ls_event("VAULT_KEY_IMPORTED", NULL, "vault key replaced (fingerprint %s); archives sent before keep their old key", fp);
        snprintf(j, sizeof(j), "{\"ok\":true,\"fingerprint\":\"%s\"}", fp);
        http_json(sock, j);
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/vault/test") == 0) {
        ls_config_t cfg;
        ls_load_config(&cfg);
        ls_vurl_t u;
        char err[220] = "", *resp = NULL;
        size_t rlen = 0;
        int st = -1;
        if (!cfg.vault_url[0] || !cfg.vault_token[0]) snprintf(err, sizeof(err), "set the vault URL and token first");
        else if (ls_vurl_parse(cfg.vault_url, &u, err, sizeof(err)) == 0) st = ls_vault_req(&u, cfg.vault_token, "GET", "/v1/ping", NULL, 0, &resp, &rlen, err, sizeof(err));
        int ok = st == 200 && resp && strstr(resp, "\"ok\": true");
        free(resp);
        char ee[440];
        json_escape(ok ? "" : (err[0] ? err : "unexpected answer"), ee, sizeof(ee));
        snprintf(j, sizeof(j), ok ? "{\"ok\":true}" : "{\"error\":\"%s\"}", ee);
        http_json(sock, j);
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/vault/sync") == 0) {
        int r = ls_worker_enqueue(LS_JOB_VAULT, "", 0, 0);
        http_json(sock, r >= 0 ? "{\"ok\":true,\"queued\":true}" : "{\"error\":\"cannot queue the job\"}");
        return 1;
    }
    http_send(sock, "404 Not Found", "application/json", LS_ERR_404, (int)strlen(LS_ERR_404));
    return 1;
}
