/*
 * LAN sync — optional login for the web UI and its API (Settings → Web UI
 * login). Included from main.c after lansync_vault.inc.c. Off by default.
 *
 * When a password is set, every /api/ request needs one of:
 *   - a session cookie from POST /api/auth/login (30 days, kept in memory:
 *     a Shallot restart asks again);
 *   - the PC client key in the header X-Shallot-Key (sync.json client_key; the
 *     PC client sends it, the downloaded config.json / docker-compose.yml
 *     carry it);
 *   - a connection from the PS5 itself (127.0.0.1: the home-screen tile).
 * The page itself always loads and asks for the password. A VPS never talks
 * to the PS5 (the PS5 or the PC push to it), so it needs nothing here.
 *
 *   GET  /api/auth/status                 {enabled, logged_in, local}
 *   POST /api/auth/login                  {"password": ...}
 *   POST /api/auth/logout
 *   POST /api/auth/password               {"current": ..., "new": ...}  ("" = turn off)
 *   GET  /api/auth/client_key             {"key": ...}
 *   POST /api/auth/client_key             new key (PC clients need the new one)
 *
 * The password is kept as a salt and 20000 rounds of HMAC-SHA256.
 */

#define LS_SESS_MAX 16
#define LS_SESS_TTL_S (30 * 86400)
#define LS_PASS_ROUNDS 20000

typedef struct { char tok[65]; int64_t until; } ls_sess_t;
static ls_sess_t g_ls_sess[LS_SESS_MAX];
static pthread_mutex_t g_ls_sess_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_ls_auth_fails;
static int64_t g_ls_auth_wait_until;

static int ls_ct_eq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned d = (unsigned)(la ^ lb);
    for (size_t i = 0; i < la && i < lb; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0 && la == lb;
}

static void ls_random_hex(char *out, size_t bytes) {
    uint8_t r[32];
    if (bytes > sizeof(r)) bytes = sizeof(r);
    ls_random(r, bytes);
    ls_hex(r, bytes, out);
}

static void ls_pass_hash(const char *salt, const char *pw, char out[65]) {
    uint8_t a[32], b[32];
    ls_hmac_sha256((const uint8_t *)salt, strlen(salt), pw, strlen(pw), a);
    for (int i = 1; i < LS_PASS_ROUNDS; i++) {
        ls_hmac_sha256((const uint8_t *)salt, strlen(salt), a, 32, b);
        memcpy(a, b, 32);
    }
    ls_hex(a, 32, out);
}

static int ls_pass_ok(const ls_config_t *cfg, const char *pw) {
    if (!cfg->ui_pass_hash[0]) return 0;
    char h[65];
    ls_pass_hash(cfg->ui_pass_salt, pw, h);
    return ls_ct_eq(h, cfg->ui_pass_hash);
}

/* Value of request header `name` (case-insensitive), or "". */
static void ls_req_header(const char *req, const char *name, char *out, size_t out_sz) {
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *p = strstr(req, "\r\n"); p && p[2] && !(p[2] == '\r' && p[3] == '\n'); p = strstr(p + 2, "\r\n")) {
        const char *h = p + 2;
        if (strncasecmp(h, name, nl) == 0 && h[nl] == ':') {
            h += nl + 1;
            while (*h == ' ') h++;
            size_t l = strcspn(h, "\r\n");
            if (l >= out_sz) l = out_sz - 1;
            memcpy(out, h, l);
            out[l] = 0;
            return;
        }
    }
}

static int ls_sess_valid(const char *req) {
    char cookie[600], tok[80] = "";
    ls_req_header(req, "Cookie", cookie, sizeof(cookie));
    const char *p = strstr(cookie, "gsid=");
    if (!p) return 0;
    p += 5;
    size_t l = strcspn(p, "; ");
    if (l != 64) return 0;
    memcpy(tok, p, 64);
    tok[64] = 0;
    int64_t now = (int64_t)time(NULL);
    int ok = 0;
    pthread_mutex_lock(&g_ls_sess_mtx);
    for (int i = 0; i < LS_SESS_MAX; i++)
        if (g_ls_sess[i].tok[0] && g_ls_sess[i].until > now && ls_ct_eq(g_ls_sess[i].tok, tok)) ok = 1;
    pthread_mutex_unlock(&g_ls_sess_mtx);
    return ok;
}

static void ls_sess_drop(const char *req) {
    char cookie[600];
    ls_req_header(req, "Cookie", cookie, sizeof(cookie));
    const char *p = strstr(cookie, "gsid=");
    if (!p) return;
    pthread_mutex_lock(&g_ls_sess_mtx);
    for (int i = 0; i < LS_SESS_MAX; i++)
        if (g_ls_sess[i].tok[0] && strncmp(g_ls_sess[i].tok, p + 5, 64) == 0) memset(&g_ls_sess[i], 0, sizeof(g_ls_sess[i]));
    pthread_mutex_unlock(&g_ls_sess_mtx);
}

static void ls_sess_new(char tok[65]) {
    ls_random_hex(tok, 32);
    int64_t now = (int64_t)time(NULL);
    pthread_mutex_lock(&g_ls_sess_mtx);
    int slot = 0;
    for (int i = 0; i < LS_SESS_MAX; i++) {
        if (!g_ls_sess[i].tok[0] || g_ls_sess[i].until <= now) { slot = i; break; }
        if (g_ls_sess[i].until < g_ls_sess[slot].until) slot = i;
    }
    snprintf(g_ls_sess[slot].tok, sizeof(g_ls_sess[slot].tok), "%s", tok);
    g_ls_sess[slot].until = now + LS_SESS_TTL_S;
    pthread_mutex_unlock(&g_ls_sess_mtx);
}

static void ls_sess_clear_all(void) {
    pthread_mutex_lock(&g_ls_sess_mtx);
    memset(g_ls_sess, 0, sizeof(g_ls_sess));
    pthread_mutex_unlock(&g_ls_sess_mtx);
}

static int ls_peer_is_local(int sock) {
    char addr[48];
    ls_peer_addr(sock, addr, sizeof(addr));
    return strcmp(addr, "127.0.0.1") == 0;
}

static void ls_auth_reply(int sock, const char *status, const char *json, const char *cookie) {
    char hdr[600];
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                      "%s%s%sConnection: close\r\n\r\n", status, strlen(json),
                      cookie ? "Set-Cookie: " : "", cookie ? cookie : "", cookie ? "\r\n" : "");
    send(sock, hdr, hl, 0);
    send(sock, json, strlen(json), 0);
}

/* JSON body of an auth request (small), or NULL. Caller frees. */
static char *ls_auth_body(int sock, char *req, int n) {
    char *body = NULL;
    size_t blen = 0;
    if (ls_read_body(sock, req, n, 4096, &body, &blen) < 0) return NULL;
    return body;
}

static int ls_auth_route(int sock, const char *method, const char *url, char *req, int n, int allowed, int local) {
    ls_config_t cfg;
    ls_load_config(&cfg);
    int enabled = cfg.ui_pass_hash[0] != 0;
    char j[300];
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/auth/status") == 0) {
        snprintf(j, sizeof(j), "{\"enabled\":%s,\"logged_in\":%s,\"local\":%s,\"client_key_set\":%s}", enabled ? "true" : "false",
                 enabled && ls_sess_valid(req) ? "true" : "false", local ? "true" : "false", cfg.client_key[0] ? "true" : "false");
        http_json(sock, j);
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/auth/login") == 0) {
        char *body = ls_auth_body(sock, req, n), pw[200] = "";
        if (body) json_get_string(body, "password", pw, sizeof(pw));
        free(body);
        int64_t now = (int64_t)time(NULL);
        if (now < g_ls_auth_wait_until) { http_json(sock, "{\"error\":\"too many attempts; wait a moment\"}"); return 1; }
        if (!enabled || !ls_pass_ok(&cfg, pw)) {
            if (++g_ls_auth_fails >= 5) { g_ls_auth_wait_until = now + 30; g_ls_auth_fails = 0; }
            sleep(1);
            ls_event("UI_LOGIN_FAILED", NULL, "wrong password from the web UI");
            http_json(sock, "{\"error\":\"wrong password\"}");
            return 1;
        }
        g_ls_auth_fails = 0;
        char tok[65], cookie[160];
        ls_sess_new(tok);
        snprintf(cookie, sizeof(cookie), "gsid=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Strict", tok, LS_SESS_TTL_S);
        ls_auth_reply(sock, "200 OK", "{\"ok\":true}", cookie);
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/auth/logout") == 0) {
        ls_sess_drop(req);
        ls_auth_reply(sock, "200 OK", "{\"ok\":true}", "gsid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
        return 1;
    }
    if (!allowed) {
        ls_auth_reply(sock, "401 Unauthorized", "{\"error\":\"login required\",\"login\":true}", NULL);
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/auth/password") == 0) {
        char *body = ls_auth_body(sock, req, n), cur[200] = "", nw[200] = "";
        if (body) {
            json_get_string(body, "current", cur, sizeof(cur));
            json_get_string(body, "new", nw, sizeof(nw));
        }
        free(body);
        pthread_mutex_lock(&g_ls_cfg_mtx);
        ls_load_config(&cfg);
        enabled = cfg.ui_pass_hash[0] != 0;
        if (enabled && !local && !ls_pass_ok(&cfg, cur)) {
            pthread_mutex_unlock(&g_ls_cfg_mtx);
            sleep(1);
            http_json(sock, "{\"error\":\"the current password is wrong\"}");
            return 1;
        }
        if (nw[0] && strlen(nw) < 6) {
            pthread_mutex_unlock(&g_ls_cfg_mtx);
            http_json(sock, "{\"error\":\"use at least 6 characters\"}");
            return 1;
        }
        if (nw[0]) {
            ls_random_hex(cfg.ui_pass_salt, 16);
            ls_pass_hash(cfg.ui_pass_salt, nw, cfg.ui_pass_hash);
            if (!cfg.client_key[0]) ls_random_hex(cfg.client_key, 24);
        } else {
            cfg.ui_pass_hash[0] = cfg.ui_pass_salt[0] = 0;
        }
        int ok = ls_save_config(&cfg) == 0;
        pthread_mutex_unlock(&g_ls_cfg_mtx);
        if (!ok) { http_json(sock, "{\"error\":\"Could not write sync.json\"}"); return 1; }
        ls_sess_clear_all();                           /* every browser logs in again */
        ls_event("UI_LOGIN_CHANGED", NULL, nw[0] ? "web UI login set" : "web UI login turned off");
        if (!nw[0]) { http_json(sock, "{\"ok\":true,\"enabled\":false}"); return 1; }
        char tok[65], cookie[160];                     /* this browser stays logged in */
        ls_sess_new(tok);
        snprintf(cookie, sizeof(cookie), "gsid=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Strict", tok, LS_SESS_TTL_S);
        ls_auth_reply(sock, "200 OK", "{\"ok\":true,\"enabled\":true}", cookie);
        return 1;
    }
    if (strcmp(url, "/api/auth/client_key") == 0) {
        if (strcmp(method, "POST") == 0) {
            pthread_mutex_lock(&g_ls_cfg_mtx);
            ls_load_config(&cfg);
            ls_random_hex(cfg.client_key, 24);
            int ok = ls_save_config(&cfg) == 0;
            pthread_mutex_unlock(&g_ls_cfg_mtx);
            if (!ok) { http_json(sock, "{\"error\":\"Could not write sync.json\"}"); return 1; }
            ls_event("UI_LOGIN_CHANGED", NULL, "new PC client key");
        }
        snprintf(j, sizeof(j), "{\"key\":\"%s\"}", cfg.client_key);
        http_json(sock, j);
        return 1;
    }
    http_send(sock, "404 Not Found", "application/json", LS_ERR_404, (int)strlen(LS_ERR_404));
    return 1;
}

/*
 * Called for every /api/ request before anything else. Returns 1 to go on,
 * 0 when it already answered (an /api/auth/ route, or 401).
 */
static int ls_auth_gate(int sock, const char *method, const char *url, char *req, int n) {
    ls_config_t cfg;
    ls_load_config(&cfg);
    int enabled = cfg.ui_pass_hash[0] != 0, local = ls_peer_is_local(sock);
    int allowed = !enabled || local || ls_sess_valid(req);
    if (!allowed && cfg.client_key[0]) {
        char key[80];
        ls_req_header(req, "X-Shallot-Key", key, sizeof(key));
        allowed = key[0] && ls_ct_eq(key, cfg.client_key);
    }
    if (strncmp(url, "/api/auth/", 10) == 0) return !ls_auth_route(sock, method, url, req, n, allowed, local);
    if (allowed) return 1;
    ls_auth_reply(sock, "401 Unauthorized", "{\"error\":\"login required\",\"login\":true}", NULL);
    return 0;
}
