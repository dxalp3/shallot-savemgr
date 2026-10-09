/*
 * LAN save sync — core (persistent state DB, durable backup queue, event log).
 * Included from main.c after the cloud staging helpers; relies on main.c's
 * logprintf, read_file_limited, json_get_*, sanitize_filename, recursive_mkdir,
 * cloud_timestamp, BACKUP_TMP_DIR and BUF_SIZE.
 *
 * Design notes (see docs/lan-sync/INVESTIGATION.md):
 *  - All state lives in /data/shallot/state/shallot_state.db (SQLite, rollback
 *    journal, synchronous=FULL). Every access holds g_ls_mtx.
 *  - Queue archives live in /data/shallot/queue/<id>.zip. They are written as
 *    <id>.zip.tmp, fsync'd, then renamed; a row is inserted only after the
 *    rename. Startup adopts orphan files and never deletes a finished .zip
 *    that has not been acknowledged.
 */

#ifndef LS_BASE_DIR
#define LS_BASE_DIR    "/data/shallot"      /* overridden by host tests */
#endif
#define LS_STATE_DIR   LS_BASE_DIR "/state"
#define LS_DB_PATH     LS_STATE_DIR "/shallot_state.db"
#define LS_QUEUE_DIR   LS_BASE_DIR "/queue"
#define LS_CONFIG_PATH LS_BASE_DIR "/sync.json"
#define LS_SCHEMA_VERSION 12
/* Shown in /api/backup/status so the owner can tell which build is running. */
#define LS_BUILD "lansync " __DATE__ " " __TIME__
#define LS_EVENTS_KEEP 2000

/* ── SHA-256 (FIPS 180-4) ───────────────────────────────────────── */
typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t used;
} ls_sha256_t;

static const uint32_t ls_sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define LS_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void ls_sha256_block(ls_sha256_t *c, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = LS_ROR(w[i-15], 7) ^ LS_ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = LS_ROR(w[i-2], 17) ^ LS_ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = LS_ROR(e, 6) ^ LS_ROR(e, 11) ^ LS_ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + ls_sha256_k[i] + w[i];
        uint32_t S0 = LS_ROR(a, 2) ^ LS_ROR(a, 13) ^ LS_ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void ls_sha256_init(ls_sha256_t *c) {
    static const uint32_t iv[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    memcpy(c->h, iv, sizeof(iv));
    c->len = 0;
    c->used = 0;
}

static void ls_sha256_update(ls_sha256_t *c, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    c->len += n;
    while (n > 0) {
        size_t take = 64 - c->used;
        if (take > n) take = n;
        memcpy(c->buf + c->used, p, take);
        c->used += take; p += take; n -= take;
        if (c->used == 64) { ls_sha256_block(c, c->buf); c->used = 0; }
    }
}

static void ls_sha256_final_hex(ls_sha256_t *c, char out[65]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    ls_sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->used != 56) ls_sha256_update(c, &zero, 1);
    uint8_t lenbe[8];
    for (int i = 0; i < 8; i++) lenbe[i] = (uint8_t)(bits >> (56 - 8 * i));
    ls_sha256_update(c, lenbe, 8);
    for (int i = 0; i < 8; i++)
        snprintf(out + i * 8, 9, "%08x", c->h[i]);
    out[64] = 0;
}

/* Hash an existing file. Returns 0 and fills hex/size on success. */
static int ls_sha256_file(const char *path, char hex[65], int64_t *size_out, char *iobuf) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ls_sha256_t c;
    ls_sha256_init(&c);
    int64_t total = 0;
    ssize_t nr;
    while ((nr = read(fd, iobuf, BUF_SIZE)) > 0) {
        ls_sha256_update(&c, iobuf, (size_t)nr);
        total += nr;
    }
    close(fd);
    if (nr < 0) return -1;
    ls_sha256_final_hex(&c, hex);
    if (size_out) *size_out = total;
    return 0;
}

/* ── Config (/data/shallot/sync.json) ─────────────────────────────── */
typedef struct {
    int enabled;                 /* LAN backup queue on/off */
    int max_queue_mb;            /* soft cap for queue storage */
    int keep_acked_hours;        /* PC-acked archives older than this may be pruned */
    /* Later stages (parsed now so the schema is stable): */
    int game_close_backup;
    int stability_check;
    int stability_interval_s;
    int stability_samples;
    int stability_timeout_s;
    int idle_sweep;
    int idle_delay_min;
    char bidirectional[16];      /* off | ask | auto */
    char regression_recovery[16];/* off | ask | certain */
    int rollback_before_restore;
    int startup_reconcile;          /* reconcile saves after (re)start */
    int startup_protect_untracked;  /* also package titles Shallot never protected */
    int followup_rescan_s;          /* re-check a closed title later (0 = off) */
    int system_probe;               /* stage O: observe rest/shutdown */
    int power_last_chance;          /* stage P1: react to a power-off signal */
    int home_tile;                  /* add the home-screen tile at start when missing */
    /* Where the backup history goes ("storage_mode"):
     *   "pc"   a PC collects every archive (default)
     *   "ps5"  the PS5 keeps it (thin + dedup), nothing leaves the console
     *   "vps"  the PS5 uploads to a vault directly, no PC
     *   "both" a PC collects and a vault holds an off-site copy; "vps_route"
     *          says who uploads to the vault: "ps5" (this console) or "pc"
     *          (the PC client relays, using the vault settings below). */
    int storage_ps5;                /* "ps5" */
    int storage_pc;                 /* "pc" or "both": the PS5 keeps an archive until the PC has it */
    int storage_vps;                /* "vps" or "both"+"ps5": the PS5 uploads to the vault itself */
    int vps_via_pc;                 /* "both"+"pc": the PC client uploads (gets url/token/key from here) */
    char pc_active[40];             /* id of the one PC client that collects ("" = the next one that connects) */
    char pc_blocked[200];           /* ids of PC clients that get nothing, comma separated */
    char ui_pass_salt[33], ui_pass_hash[65];   /* web UI login ("" = off), lansync_auth.inc.c */
    char client_key[65];            /* PC clients send it (X-Shallot-Key) while the login is on */
    char vault_url[160];            /* http://host:8091 (the vault's signed port) */
    char vault_token[100];
    int ps5_keep_all_days, ps5_daily_days, ps5_weekly;   /* history thinning: PS5 only, and the vault when the PS5 uploads */
    int vault_keep_all;             /* the PS5 never thins the vault */
} ls_config_t;

static void ls_config_defaults(ls_config_t *c) {
    memset(c, 0, sizeof(*c));
    c->enabled = 1;
    c->max_queue_mb = 2048;
    c->keep_acked_hours = 168;
    c->game_close_backup = 1;
    c->stability_check = 1;
    c->stability_interval_s = 2;
    c->stability_samples = 2;
    c->stability_timeout_s = 45;
    c->idle_sweep = 1;
    c->idle_delay_min = 10;
    snprintf(c->bidirectional, sizeof(c->bidirectional), "ask");
    snprintf(c->regression_recovery, sizeof(c->regression_recovery), "ask");
    c->rollback_before_restore = 1;
    c->startup_reconcile = 1;
    /* Owner preference (2026-10-07): back up games Shallot has never
     * protected once at startup ("everything, just in case"). */
    c->startup_protect_untracked = 1;
    /* The PS5 refreshes its sce_bu_* backup copies some time after a title
     * exits (observed on 13.60), after the stability window has passed. */
    c->followup_rescan_s = 180;
    c->system_probe = 1;
    c->power_last_chance = 1;
    c->home_tile = 1;
    c->storage_ps5 = 0;
    c->storage_pc = 1;
    c->storage_vps = 0;
    c->vps_via_pc = 0;
    c->vault_url[0] = 0;
    c->vault_token[0] = 0;
    c->ps5_keep_all_days = 2;
    c->ps5_daily_days = 30;
    c->ps5_weekly = 1;
}

/* Applies the fields present in `json` on top of *c (unknown keys ignored). */
static void ls_parse_config(const char *json, ls_config_t *c) {
    c->enabled = json_get_bool_default(json, "enabled", c->enabled);
    c->max_queue_mb = json_get_int_default(json, "max_queue_mb", c->max_queue_mb);
    c->keep_acked_hours = json_get_int_default(json, "keep_acked_hours", c->keep_acked_hours);
    c->game_close_backup = json_get_bool_default(json, "game_close_backup", c->game_close_backup);
    c->stability_check = json_get_bool_default(json, "stability_check", c->stability_check);
    c->stability_interval_s = json_get_int_default(json, "stability_interval_s", c->stability_interval_s);
    c->stability_samples = json_get_int_default(json, "stability_samples", c->stability_samples);
    c->stability_timeout_s = json_get_int_default(json, "stability_timeout_s", c->stability_timeout_s);
    c->idle_sweep = json_get_bool_default(json, "idle_sweep", c->idle_sweep);
    c->idle_delay_min = json_get_int_default(json, "idle_delay_min", c->idle_delay_min);
    json_get_string(json, "bidirectional", c->bidirectional, sizeof(c->bidirectional));
    json_get_string(json, "regression_recovery", c->regression_recovery, sizeof(c->regression_recovery));
    c->rollback_before_restore = json_get_bool_default(json, "rollback_before_restore", c->rollback_before_restore);
    c->startup_reconcile = json_get_bool_default(json, "startup_reconcile", c->startup_reconcile);
    c->startup_protect_untracked = json_get_bool_default(json, "startup_protect_untracked", c->startup_protect_untracked);
    c->followup_rescan_s = json_get_int_default(json, "followup_rescan_s", c->followup_rescan_s);
    c->system_probe = json_get_bool_default(json, "system_probe", c->system_probe);
    c->power_last_chance = json_get_bool_default(json, "power_last_chance", c->power_last_chance);
    c->home_tile = json_get_bool_default(json, "home_tile", c->home_tile);
    {
        char mode[16] = "", route[16] = "";
        json_get_string(json, "storage_mode", mode, sizeof(mode));
        json_get_string(json, "vps_route", route, sizeof(route));
        if (mode[0]) {
            int both = strcmp(mode, "both") == 0;
            c->storage_ps5 = strcmp(mode, "ps5") == 0;
            c->storage_pc = both || (!c->storage_ps5 && strcmp(mode, "vps") != 0);
            c->vps_via_pc = both && strcmp(route, "pc") == 0;
            c->storage_vps = strcmp(mode, "vps") == 0 || (both && !c->vps_via_pc);
        }
    }
    json_get_string(json, "pc_active", c->pc_active, sizeof(c->pc_active));
    json_get_string(json, "ui_pass_salt", c->ui_pass_salt, sizeof(c->ui_pass_salt));
    json_get_string(json, "ui_pass_hash", c->ui_pass_hash, sizeof(c->ui_pass_hash));
    json_get_string(json, "client_key", c->client_key, sizeof(c->client_key));
    json_get_string(json, "pc_blocked", c->pc_blocked, sizeof(c->pc_blocked));
    json_get_string(json, "vault_url", c->vault_url, sizeof(c->vault_url));
    json_get_string(json, "vault_token", c->vault_token, sizeof(c->vault_token));
    c->ps5_keep_all_days = json_get_int_default(json, "ps5_keep_all_days", c->ps5_keep_all_days);
    c->ps5_daily_days = json_get_int_default(json, "ps5_daily_days", c->ps5_daily_days);
    c->ps5_weekly = json_get_bool_default(json, "ps5_weekly", c->ps5_weekly);
    c->vault_keep_all = json_get_bool_default(json, "vault_keep_all", c->vault_keep_all);
    if (c->ps5_keep_all_days < 0) c->ps5_keep_all_days = 0;
    if (c->ps5_daily_days < c->ps5_keep_all_days) c->ps5_daily_days = c->ps5_keep_all_days;
}

static void ls_config_clamp(ls_config_t *c) {
    if (c->max_queue_mb < 64) c->max_queue_mb = 64;
    if (c->keep_acked_hours < 1) c->keep_acked_hours = 1;
    if (c->stability_interval_s < 1) c->stability_interval_s = 1;
    if (c->stability_samples < 1) c->stability_samples = 1;
    if (c->stability_timeout_s < 5) c->stability_timeout_s = 5;
    if (c->idle_delay_min < 1) c->idle_delay_min = 1;
    if (c->followup_rescan_s < 0) c->followup_rescan_s = 0;
    if (c->stability_samples > 20) c->stability_samples = 20;
    if (c->stability_interval_s > 60) c->stability_interval_s = 60;
    if (c->stability_timeout_s > 600) c->stability_timeout_s = 600;
    for (char *p = c->pc_active; *p; p++) if (!isalnum((unsigned char)*p) && !strchr(":._-", *p)) *p = '_';
    for (char *p = c->ui_pass_salt; *p; p++) if (!isxdigit((unsigned char)*p)) *p = '0';
    for (char *p = c->ui_pass_hash; *p; p++) if (!isxdigit((unsigned char)*p)) *p = '0';
    for (char *p = c->client_key; *p; p++) if (!isxdigit((unsigned char)*p)) *p = '0';
    for (char *p = c->pc_blocked; *p; p++) if (!isalnum((unsigned char)*p) && !strchr(":._-,", *p)) *p = '_';
    if (strcmp(c->bidirectional, "off") && strcmp(c->bidirectional, "ask") && strcmp(c->bidirectional, "auto"))
        snprintf(c->bidirectional, sizeof(c->bidirectional), "ask");
    if (strcmp(c->regression_recovery, "off") && strcmp(c->regression_recovery, "ask") &&
        strcmp(c->regression_recovery, "certain"))
        snprintf(c->regression_recovery, sizeof(c->regression_recovery), "ask");
}

static void ls_load_config(ls_config_t *c) {
    ls_config_defaults(c);
    char *json = NULL;
    if (read_file_limited(LS_CONFIG_PATH, &json, 16 * 1024) < 0 || !json) { ls_config_clamp(c); return; }
    ls_parse_config(json, c);
    free(json);
    ls_config_clamp(c);
}

static const char *ls_storage_mode_name(const ls_config_t *c) {
    if (c->storage_ps5) return "ps5";
    if (c->storage_pc && (c->storage_vps || c->vps_via_pc)) return "both";
    if (c->storage_vps) return "vps";
    return "pc";
}

static int ls_config_to_json(const ls_config_t *c, char *out, size_t n) {
    char vurl[340], vtok[220];
    json_escape(c->vault_url, vurl, sizeof(vurl));
    json_escape(c->vault_token, vtok, sizeof(vtok));
    return snprintf(out, n,
        "{\n  \"enabled\": %s,\n  \"max_queue_mb\": %d,\n  \"keep_acked_hours\": %d,\n"
        "  \"game_close_backup\": %s,\n  \"stability_check\": %s,\n  \"stability_interval_s\": %d,\n"
        "  \"stability_samples\": %d,\n  \"stability_timeout_s\": %d,\n  \"followup_rescan_s\": %d,\n"
        "  \"idle_sweep\": %s,\n  \"idle_delay_min\": %d,\n  \"startup_reconcile\": %s,\n"
        "  \"startup_protect_untracked\": %s,\n  \"bidirectional\": \"%s\",\n"
        "  \"regression_recovery\": \"%s\",\n  \"rollback_before_restore\": %s,\n  \"system_probe\": %s,\n  \"power_last_chance\": %s,\n  \"home_tile\": %s,\n  \"storage_mode\": \"%s\",\n  \"vps_route\": \"%s\",\n  \"pc_active\": \"%s\",\n  \"pc_blocked\": \"%s\",\n"
        "  \"ui_pass_salt\": \"%s\",\n  \"ui_pass_hash\": \"%s\",\n  \"client_key\": \"%s\",\n"
        "  \"ps5_keep_all_days\": %d,\n  \"ps5_daily_days\": %d,\n  \"ps5_weekly\": %s,\n  \"vault_keep_all\": %s,\n"
        "  \"vault_url\": \"%s\",\n  \"vault_token\": \"%s\"\n}\n",
        c->enabled ? "true" : "false", c->max_queue_mb, c->keep_acked_hours,
        c->game_close_backup ? "true" : "false", c->stability_check ? "true" : "false",
        c->stability_interval_s, c->stability_samples, c->stability_timeout_s, c->followup_rescan_s,
        c->idle_sweep ? "true" : "false", c->idle_delay_min, c->startup_reconcile ? "true" : "false",
        c->startup_protect_untracked ? "true" : "false", c->bidirectional, c->regression_recovery,
        c->rollback_before_restore ? "true" : "false", c->system_probe ? "true" : "false", c->power_last_chance ? "true" : "false",
        c->home_tile ? "true" : "false", ls_storage_mode_name(c), c->vps_via_pc ? "pc" : "ps5", c->pc_active, c->pc_blocked, c->ui_pass_salt, c->ui_pass_hash, c->client_key, c->ps5_keep_all_days,
        c->ps5_daily_days, c->ps5_weekly ? "true" : "false", c->vault_keep_all ? "true" : "false", vurl, vtok);
}

/* Atomic write of sync.json (tmp -> fsync -> rename). Returns 0 on success. */
/* Every read-change-write of sync.json holds this (several request threads
 * write it: settings, PC client changes, login). Plain reads need no lock:
 * a save is an atomic rename. */
static pthread_mutex_t g_ls_cfg_mtx __attribute__((unused)) = PTHREAD_MUTEX_INITIALIZER;

static int ls_save_config(const ls_config_t *c) {
    char json[4096];
    int n = ls_config_to_json(c, json, sizeof(json));
    if (n <= 0 || (size_t)n >= sizeof(json)) return -1;
    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s.tmp", LS_CONFIG_PATH);
    int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) return -1;
    int ok = write(fd, json, (size_t)n) == n && fsync(fd) == 0;
    if (close(fd) < 0) ok = 0;
    if (!ok || rename(tmp, LS_CONFIG_PATH) < 0) { unlink(tmp); return -1; }
    return 0;
}

/* Growable JSON/text builder used by the HTTP and reconcile code. */
static int ls_json_append(char **buf, size_t *cap, size_t *pos, const char *fmt, ...) {
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(*buf + *pos, *cap - *pos, fmt, ap);
        va_end(ap);
        if (n < 0) return -1;
        if (*pos + (size_t)n < *cap) { *pos += (size_t)n; return 0; }
        size_t ncap = *cap * 2 + (size_t)n;
        char *nb = realloc(*buf, ncap);
        if (!nb) return -1;
        *buf = nb;
        *cap = ncap;
    }
}

/* ── State DB ───────────────────────────────────────────────────── */
static pthread_mutex_t g_ls_mtx = PTHREAD_MUTEX_INITIALIZER;
static sqlite3 *g_ls_db = NULL;
static int g_ls_events_since_trim = 0;

static int ls_exec_locked(const char *sql) {
    char *msg = NULL;
    int rc = sqlite3_exec(g_ls_db, sql, NULL, NULL, &msg);
    if (rc != SQLITE_OK) {
        logprintf("[LanSync] SQL error: %s (%s)\n", msg ? msg : "?", sql);
        sqlite3_free(msg);
        return -1;
    }
    return 0;
}

static void ls_bind_text(sqlite3_stmt *st, int idx, const char *s) {
    sqlite3_bind_text(st, idx, s ? s : "", -1, SQLITE_TRANSIENT);
}

/* Single-integer query; binds up to 3 text params then one int (by count). Caller holds g_ls_mtx. */
static int64_t ls_db_int_locked(const char *sql, const char *a, const char *b, const char *c, int64_t d) {
    int64_t v = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK) {
        int n = sqlite3_bind_parameter_count(st);
        if (n >= 1) ls_bind_text(st, 1, a);
        if (n >= 2) ls_bind_text(st, 2, b);
        if (n >= 3) ls_bind_text(st, 3, c);
        if (n >= 4) sqlite3_bind_int64(st, 4, (sqlite3_int64)d);
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return v;
}

/* Bounded, persistent, timestamped event log. Also mirrored to logprintf. */
static void ls_event(const char *code, const char *title_id, const char *fmt, ...) {
    char detail[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    time_t now = time(NULL);
    char stamp[32];
    cloud_timestamp(stamp, sizeof(stamp));
    logprintf("[LanSync %s] %s %s %s\n", stamp, code, title_id ? title_id : "-", detail);

    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db,
                "INSERT INTO events(ts, code, title_id, detail) VALUES(?,?,?,?)",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)now);
            ls_bind_text(st, 2, code);
            ls_bind_text(st, 3, title_id);
            ls_bind_text(st, 4, detail);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
        if (++g_ls_events_since_trim >= 100) {
            g_ls_events_since_trim = 0;
            char sql[160];
            snprintf(sql, sizeof(sql),
                     "DELETE FROM events WHERE id <= (SELECT MAX(id) FROM events) - %d", LS_EVENTS_KEEP);
            ls_exec_locked(sql);
        }
    }
    pthread_mutex_unlock(&g_ls_mtx);
}

static void ls_meta_set_int(const char *key, int64_t v) {
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)",
                               -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, key);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)v);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
}

static int64_t ls_meta_get_int(const char *key, int64_t def) {
    int64_t v = def;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "SELECT value FROM meta WHERE key = ?", -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, key);
            if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    return v;
}

/* Operation journal: anything still 'running' at startup was interrupted. */
static int64_t ls_op_begin(const char *kind, const char *target) {
    int64_t id = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db,
                "INSERT INTO ops(kind, target, started_at, status) VALUES(?,?,?,'running')",
                -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, kind);
            ls_bind_text(st, 2, target);
            sqlite3_bind_int64(st, 3, (sqlite3_int64)time(NULL));
            if (sqlite3_step(st) == SQLITE_DONE) id = sqlite3_last_insert_rowid(g_ls_db);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    return id;
}

static void ls_op_end(int64_t op_id, const char *status, const char *detail) {
    if (op_id <= 0) return;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db,
                "UPDATE ops SET finished_at = ?, status = ?, detail = ? WHERE id = ?",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
            ls_bind_text(st, 2, status);
            ls_bind_text(st, 3, detail);
            sqlite3_bind_int64(st, 4, (sqlite3_int64)op_id);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
}

static const char *ls_schema_v1 =
    "CREATE TABLE IF NOT EXISTS meta("
    "  key TEXT PRIMARY KEY, value)"
    ";CREATE TABLE IF NOT EXISTS queue("
    "  id TEXT PRIMARY KEY, filename TEXT NOT NULL, title_id TEXT, save_name TEXT, user_id TEXT,"
    "  gen_id INTEGER DEFAULT 0, sha256 TEXT, size INTEGER, created_at INTEGER, reason TEXT,"
    "  state TEXT NOT NULL,"            /* ready | missing | pruned */
    "  cloud_needed INTEGER DEFAULT 0, lan_ack_at INTEGER DEFAULT 0, cloud_ack_at INTEGER DEFAULT 0,"
    "  pc_attempts INTEGER DEFAULT 0, last_error TEXT)"
    ";CREATE INDEX IF NOT EXISTS queue_state ON queue(state, created_at)"
    ";CREATE TABLE IF NOT EXISTS ops("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT, target TEXT, started_at INTEGER,"
    "  finished_at INTEGER, status TEXT, detail TEXT)"
    ";CREATE TABLE IF NOT EXISTS events("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER, code TEXT, title_id TEXT, detail TEXT)";

/* v2: per-save protection state (stage C). */
static const char *ls_schema_v2 =
    "CREATE TABLE IF NOT EXISTS saves("
    "  user_id TEXT NOT NULL, title_id TEXT NOT NULL, save_name TEXT NOT NULL,"
    "  is_ps4 INTEGER DEFAULT 0,"
    "  last_seen_meta TEXT, last_seen_at INTEGER DEFAULT 0,"
    "  last_packaged_meta TEXT, last_packaged_sha TEXT, last_packaged_at INTEGER DEFAULT 0,"
    "  last_queue_id TEXT,"
    "  state TEXT DEFAULT 'unknown',"   /* unknown|protected|changed|unstable|missing|error */
    "  last_error TEXT,"
    "  PRIMARY KEY(user_id, title_id, save_name))"
    ";CREATE INDEX IF NOT EXISTS saves_title ON saves(title_id)";

/* v3: generations = every distinct container content ever archived per save,
 * linked to its predecessor on this console (stage E). */
static const char *ls_schema_v3 =
    "CREATE TABLE IF NOT EXISTS generations("
    "  gen_id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id TEXT NOT NULL, title_id TEXT NOT NULL, save_name TEXT NOT NULL,"
    "  sha256 TEXT NOT NULL, size INTEGER, meta TEXT, parent_gen INTEGER DEFAULT 0,"
    "  created_at INTEGER, reason TEXT, queue_id TEXT,"
    "  pc_acked_at INTEGER DEFAULT 0, cloud_acked_at INTEGER DEFAULT 0)"
    ";CREATE INDEX IF NOT EXISTS generations_save ON generations(user_id, title_id, save_name, gen_id)"
    ";CREATE INDEX IF NOT EXISTS generations_sha ON generations(sha256)"
    ";ALTER TABLE saves ADD COLUMN current_gen_id INTEGER DEFAULT 0"
    /* Backfill: the last packaged content of every save becomes its first generation. */
    ";INSERT INTO generations(user_id, title_id, save_name, sha256, meta, created_at, reason, queue_id)"
    "  SELECT user_id, title_id, save_name, last_packaged_sha, last_packaged_meta, last_packaged_at,"
    "         'migrated', last_queue_id FROM saves"
    "  WHERE last_packaged_sha IS NOT NULL AND last_packaged_sha != ''"
    ";UPDATE saves SET current_gen_id = COALESCE((SELECT MAX(g.gen_id) FROM generations g"
    "  WHERE g.user_id = saves.user_id AND g.title_id = saves.title_id AND g.save_name = saves.save_name), 0)"
    ";UPDATE queue SET gen_id = (SELECT g.gen_id FROM generations g WHERE g.queue_id = queue.id)"
    "  WHERE gen_id = 0 AND id IN (SELECT queue_id FROM generations)"
    ";UPDATE generations SET"
    "  pc_acked_at = COALESCE((SELECT q.lan_ack_at FROM queue q WHERE q.id = generations.queue_id), 0),"
    "  cloud_acked_at = COALESCE((SELECT q.cloud_ack_at FROM queue q WHERE q.id = generations.queue_id), 0)";

/* v4: last manual reconciliation result per title (stage G+). */
static const char *ls_schema_v4 =
    "CREATE TABLE IF NOT EXISTS reconcile("
    "  title_id TEXT PRIMARY KEY,"
    "  last_sanity_check INTEGER DEFAULT 0, last_full_reconcile INTEGER DEFAULT 0,"
    "  last_catalogue_scan INTEGER DEFAULT 0, last_result TEXT, last_known_current_fp TEXT,"
    "  last_known_pc_generation INTEGER DEFAULT 0, last_common_generation INTEGER DEFAULT 0,"
    "  recommended_action TEXT, last_report TEXT)";

/* v5: what the PC client reports it holds, per title (stage L). */
static const char *ls_schema_v5 =
    "CREATE TABLE IF NOT EXISTS pc_manifest("
    "  title_id TEXT NOT NULL, archive_sha256 TEXT NOT NULL, save_name TEXT, user_id TEXT,"
    "  container_sha256 TEXT, queue_id TEXT, created_at INTEGER DEFAULT 0, origin TEXT,"
    "  gen_id INTEGER DEFAULT 0, received_at INTEGER DEFAULT 0,"
    "  PRIMARY KEY(title_id, archive_sha256, save_name))"
    ";CREATE INDEX IF NOT EXISTS pc_manifest_save ON pc_manifest(title_id, save_name)"
    ";CREATE TABLE IF NOT EXISTS pc_titles(title_id TEXT PRIMARY KEY, received_at INTEGER, archives INTEGER)";

/* v6: snapshots = which archived generation of every file of a game (kind
 * 'title') or of one save object (kind 'object') belonged together at one
 * stable point in time. object_id/role are derived from the file name
 * (sdimg_<X> primary, sdimg_sce_bu_<X> sce_backup). Backfill: the last
 * protected state of every (user, title) becomes one 'migrated' snapshot. */
#define LS_SQL_OBJECT_ID(col) \
    "CASE WHEN " col " LIKE 'sdimg\\_sce\\_bu\\__%' ESCAPE '\\' THEN substr(" col ", 14)" \
    " WHEN " col " LIKE 'sdimg\\__%' ESCAPE '\\' THEN substr(" col ", 7)" \
    " WHEN " col " LIKE 'sce\\_bu\\__%' ESCAPE '\\' THEN substr(" col ", 8) ELSE " col " END"
#define LS_SQL_ROLE(col) \
    "CASE WHEN " col " LIKE 'sdimg\\_sce\\_bu\\__%' ESCAPE '\\' OR " col " LIKE 'sce\\_bu\\__%' ESCAPE '\\'" \
    " THEN 'sce_backup' ELSE 'primary' END"
static const char *ls_schema_v6 =
    "CREATE TABLE IF NOT EXISTS snapshots("
    "  snap_id INTEGER PRIMARY KEY AUTOINCREMENT, user_id TEXT NOT NULL, title_id TEXT NOT NULL,"
    "  kind TEXT NOT NULL, object_id TEXT DEFAULT '', created_at INTEGER, reason TEXT,"
    "  objects INTEGER DEFAULT 0, files INTEGER DEFAULT 0, fingerprint TEXT DEFAULT '')"
    ";CREATE INDEX IF NOT EXISTS snapshots_title ON snapshots(title_id, user_id, kind, snap_id)"
    ";CREATE TABLE IF NOT EXISTS snapshot_files("
    "  snap_id INTEGER NOT NULL, object_id TEXT NOT NULL, role TEXT NOT NULL, save_name TEXT NOT NULL,"
    "  is_ps4 INTEGER DEFAULT 0, gen_id INTEGER DEFAULT 0, sha256 TEXT, size INTEGER DEFAULT 0,"
    "  PRIMARY KEY(snap_id, save_name))"
    ";INSERT INTO snapshots(user_id, title_id, kind, object_id, created_at, reason)"
    "  SELECT user_id, title_id, 'title', '', MAX(last_packaged_at), 'migrated' FROM saves"
    "  WHERE current_gen_id > 0 AND state != 'missing' GROUP BY user_id, title_id"
    ";INSERT INTO snapshot_files(snap_id, object_id, role, save_name, is_ps4, gen_id, sha256, size)"
    "  SELECT n.snap_id, " LS_SQL_OBJECT_ID("s.save_name") ", " LS_SQL_ROLE("s.save_name") ","
    "         s.save_name, s.is_ps4, s.current_gen_id, g.sha256, COALESCE(g.size, 0)"
    "  FROM saves s JOIN snapshots n ON n.user_id = s.user_id AND n.title_id = s.title_id AND n.reason = 'migrated'"
    "  JOIN generations g ON g.gen_id = s.current_gen_id"
    "  WHERE s.current_gen_id > 0 AND s.state != 'missing'"
    ";UPDATE snapshots SET"
    "  files = (SELECT COUNT(*) FROM snapshot_files f WHERE f.snap_id = snapshots.snap_id),"
    "  objects = (SELECT COUNT(DISTINCT object_id) FROM snapshot_files f WHERE f.snap_id = snapshots.snap_id)"
    "  WHERE reason = 'migrated'";

/* v7: regression recovery offers (stage N), one open offer per save object. */
static const char *ls_schema_v7 =
    "CREATE TABLE IF NOT EXISTS recovery("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, user_id TEXT, title_id TEXT NOT NULL, object_id TEXT NOT NULL,"
    "  detected_at INTEGER, reason TEXT, target_snap INTEGER DEFAULT 0, files TEXT, certain INTEGER DEFAULT 0,"
    "  target_on_ps5 INTEGER DEFAULT 0, target_on_pc INTEGER DEFAULT 0,"
    "  state TEXT NOT NULL,"            /* open | kept | restored | auto_restore | superseded */
    "  stale INTEGER DEFAULT 0, resolved_at INTEGER DEFAULT 0, detail TEXT)"
    ";CREATE INDEX IF NOT EXISTS recovery_title ON recovery(title_id, state)";

/* v8: display names for save objects (Browse), per game + save object. */
static const char *ls_schema_v8 =
    "CREATE TABLE IF NOT EXISTS labels("
    "  title_id TEXT NOT NULL, object_id TEXT NOT NULL, label TEXT NOT NULL, updated_at INTEGER,"
    "  PRIMARY KEY(title_id, object_id))";

/* v9: PS5-side block store (PS5-only mode, lansync_blocks.inc.c). */
static const char *ls_schema_v9 =
    "CREATE TABLE IF NOT EXISTS blocks(hash BLOB PRIMARY KEY, pack INTEGER NOT NULL, off INTEGER NOT NULL,"
    "  len INTEGER NOT NULL, refs INTEGER DEFAULT 0)"
    ";CREATE INDEX IF NOT EXISTS blocks_pack ON blocks(pack)"
    ";CREATE TABLE IF NOT EXISTS recipes(queue_id TEXT PRIMARY KEY, size INTEGER, sha256 TEXT, nblocks INTEGER,"
    "  hashes BLOB, packed_at INTEGER)"
    ";CREATE TABLE IF NOT EXISTS packs(pack INTEGER PRIMARY KEY, bytes INTEGER DEFAULT 0, dead INTEGER DEFAULT 0)";

/* v10: archives the PS5 uploaded to a vault itself (VPS only mode). */
static const char *ls_schema_v10 = "ALTER TABLE queue ADD COLUMN vault_ack_at INTEGER DEFAULT 0";
/* v11: a stand-in PC client (collecting while the primary one is away) has this archive */
static const char *ls_schema_v11 = "ALTER TABLE queue ADD COLUMN standin_ack TEXT DEFAULT ''";
/* v12: thinned out of the vault (kept here as history) */
static const char *ls_schema_v12 =
    "ALTER TABLE queue ADD COLUMN vault_gone INTEGER DEFAULT 0;"
    "ALTER TABLE snapshots ADD COLUMN vault_gone INTEGER DEFAULT 0";

static int g_ls_schema = 0;

/* Display name of a save object ("" = none). */
static void ls_label_get(const char *title_id, const char *object_id, char *out, size_t out_sz) {
    out[0] = 0;
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *st = NULL;
    if (g_ls_db && sqlite3_prepare_v2(g_ls_db, "SELECT label FROM labels WHERE title_id = ? AND object_id = ?",
                                      -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, title_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, object_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
            snprintf(out, out_sz, "%s", (const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
}

/* Sets (or with "" removes) a display name. 0 = ok. */
static int ls_label_set(const char *title_id, const char *object_id, const char *label) {
    int rc = -1;
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *st = NULL;
    const char *sql = label[0] ? "INSERT OR REPLACE INTO labels(title_id, object_id, label, updated_at) VALUES(?, ?, ?, ?)"
                               : "DELETE FROM labels WHERE title_id = ? AND object_id = ?";
    if (g_ls_db && sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, title_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, object_id, -1, SQLITE_TRANSIENT);
        if (label[0]) {
            sqlite3_bind_text(st, 3, label, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 4, (sqlite3_int64)time(NULL));
        }
        rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    return rc;
}

static int ls_user_version_locked(void) {
    int v = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_ls_db, "PRAGMA user_version", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* Forward-only migration ladder, one transaction per step. */
static int ls_migrate_locked(void) {
    static const char *steps[] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
    steps[2] = ls_schema_v2;
    steps[3] = ls_schema_v3;
    steps[4] = ls_schema_v4;
    steps[5] = ls_schema_v5;
    steps[6] = ls_schema_v6;
    steps[7] = ls_schema_v7;
    steps[8] = ls_schema_v8;
    steps[9] = ls_schema_v9;
    steps[10] = ls_schema_v10;
    steps[11] = ls_schema_v11;
    steps[12] = ls_schema_v12;
    int v = ls_user_version_locked();
    if (v < 1) v = 1;                      /* v1 tables are created idempotently */
    for (int target = v + 1; target <= LS_SCHEMA_VERSION; target++) {
        char sql[64];
        if (ls_exec_locked("BEGIN") < 0) return -1;
        if (ls_exec_locked(steps[target]) < 0) { ls_exec_locked("ROLLBACK"); return -1; }
        snprintf(sql, sizeof(sql), "PRAGMA user_version=%d", target);
        ls_exec_locked(sql);
        if (ls_exec_locked("COMMIT") < 0) { ls_exec_locked("ROLLBACK"); return -1; }
        logprintf("[LanSync] state DB migrated to schema %d\n", target);
        v = target;
    }
    g_ls_schema = v;
    return 0;
}

static int ls_open_db(void) {
#ifndef LS_HOST_TEST
    mkdir("/data", 0777);
#endif
    mkdir(LS_BASE_DIR, 0777);
    mkdir(LS_STATE_DIR, 0777);
    mkdir(LS_QUEUE_DIR, 0777);
    if (sqlite3_open_v2(LS_DB_PATH, &g_ls_db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        logprintf("[LanSync] cannot open %s: %s\n", LS_DB_PATH,
                  g_ls_db ? sqlite3_errmsg(g_ls_db) : "?");
        if (g_ls_db) sqlite3_close(g_ls_db);
        g_ls_db = NULL;
        return -1;
    }
    sqlite3_busy_timeout(g_ls_db, 5000);
    ls_exec_locked("PRAGMA journal_mode=DELETE");
    ls_exec_locked("PRAGMA synchronous=FULL");
    if (ls_exec_locked(ls_schema_v1) < 0) return -1;
    /* Pre-ladder builds set user_version=1 then 2 on every start; the saves
     * table may already exist at "v1", which v2's IF NOT EXISTS tolerates. */
    if (ls_user_version_locked() == 0) ls_exec_locked("PRAGMA user_version=1");
    return ls_migrate_locked();
}

/* ── Durable file helpers ───────────────────────────────────────── */
static int ls_write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t w = write(fd, buf, len);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        buf += w;
        len -= (size_t)w;
    }
    return 0;
}

/* Copy src -> dst_tmp with checked writes + fsync, hashing on the way. */
static int ls_copy_hash(const char *src, const char *dst_tmp, char hex[65],
                        int64_t *size_out, char *iobuf) {
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) return -1;
    int dfd = open(dst_tmp, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (dfd < 0) { close(sfd); return -2; }
    ls_sha256_t c;
    ls_sha256_init(&c);
    int64_t total = 0;
    ssize_t nr;
    int rc = 0;
    while ((nr = read(sfd, iobuf, BUF_SIZE)) > 0) {
        if (ls_write_all(dfd, iobuf, (size_t)nr) < 0) { rc = -3; break; }
        ls_sha256_update(&c, iobuf, (size_t)nr);
        total += nr;
    }
    if (nr < 0) rc = -4;
    if (rc == 0 && fsync(dfd) < 0) rc = -5;
    close(sfd);
    if (close(dfd) < 0 && rc == 0) rc = -6;
    if (rc < 0) { unlink(dst_tmp); return rc; }
    ls_sha256_final_hex(&c, hex);
    if (size_out) *size_out = total;
    return 0;
}

static int ls_has_suffix(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

/* Queue ids must be safe for safe_queue_id(): no '.', '/', '\\', ':'. */
static void ls_make_queue_id(const char *title_id, const char *save_name, char *out, size_t out_sz) {
    static unsigned int seq = 0;
    char stamp[32], t[40], s[96];
    cloud_timestamp(stamp, sizeof(stamp));
    snprintf(t, sizeof(t), "%s", title_id && title_id[0] ? title_id : "UNKNOWN");
    snprintf(s, sizeof(s), "%s", save_name && save_name[0] ? save_name : "all");
    for (char *p = t; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') *p = '_';
    for (char *p = s; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') *p = '_';
    seq = (seq + 1) % 10000;
    snprintf(out, out_sz, "%s_%s_%s_%04u", stamp, t, s, seq);
}

typedef struct {
    char title_id[32];
    char save_name[256];
    char user_id[16];
    char reason[32];
    int64_t gen_id;
    int cloud_needed;
} ls_queue_meta_t;

static int ls_queue_insert_row(const char *id, const char *filename, const ls_queue_meta_t *m,
                               const char *sha, int64_t size) {
    int rc = -1;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db,
                "INSERT INTO queue(id, filename, title_id, save_name, user_id, gen_id, sha256, size,"
                " created_at, reason, state, cloud_needed) VALUES(?,?,?,?,?,?,?,?,?,?,'ready',?)",
                -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, id);
            ls_bind_text(st, 2, filename);
            ls_bind_text(st, 3, m->title_id);
            ls_bind_text(st, 4, m->save_name);
            ls_bind_text(st, 5, m->user_id);
            sqlite3_bind_int64(st, 6, (sqlite3_int64)m->gen_id);
            ls_bind_text(st, 7, sha);
            sqlite3_bind_int64(st, 8, (sqlite3_int64)size);
            sqlite3_bind_int64(st, 9, (sqlite3_int64)time(NULL));
            ls_bind_text(st, 10, m->reason);
            sqlite3_bind_int(st, 11, m->cloud_needed ? 1 : 0);
            if (sqlite3_step(st) == SQLITE_DONE) rc = 0;
            else logprintf("[LanSync] queue insert failed: %s\n", sqlite3_errmsg(g_ls_db));
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    return rc;
}

/*
 * PC clients wait in GET /api/backup/wait (lansync_pclink.inc.c) until this
 * sequence moves: a new archive was queued, or someone asked to sync now.
 */
static pthread_mutex_t g_ls_wait_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ls_wait_cv = PTHREAD_COND_INITIALIZER;
static int64_t g_ls_wait_seq = 1;
static char g_ls_wait_reason[16] = "start";
static void ls_wait_bump(const char *reason) {
    pthread_mutex_lock(&g_ls_wait_mtx);
    g_ls_wait_seq++;
    snprintf(g_ls_wait_reason, sizeof(g_ls_wait_reason), "%s", reason);
    pthread_cond_broadcast(&g_ls_wait_cv);
    pthread_mutex_unlock(&g_ls_wait_mtx);
}

/*
 * Add a finished archive to the persistent queue. When move_src is set the
 * source is renamed into the queue (same filesystem); otherwise it is copied
 * durably. On success out_id holds the queue id.
 */
static int ls_queue_add(const char *src_path, int move_src, const ls_queue_meta_t *m,
                        char *out_id, size_t out_id_sz, char *iobuf,
                        char *err, size_t err_sz) {
    char id[200], final_path[MAX_PATH_LEN], tmp_path[MAX_PATH_LEN], filename[220];
    char hex[65] = {0};
    int64_t size = 0;
    mkdir(LS_QUEUE_DIR, 0777);
    ls_make_queue_id(m->title_id, m->save_name, id, sizeof(id));
    snprintf(filename, sizeof(filename), "%s.zip", id);
    snprintf(final_path, sizeof(final_path), "%s/%s", LS_QUEUE_DIR, filename);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);

    if (move_src) {
        if (ls_sha256_file(src_path, hex, &size, iobuf) < 0) {
            snprintf(err, err_sz, "Cannot hash archive");
            return -1;
        }
        if (rename(src_path, final_path) < 0) {
            snprintf(err, err_sz, "Cannot move archive into queue (errno %d)", errno);
            return -1;
        }
    } else {
        int cr = ls_copy_hash(src_path, tmp_path, hex, &size, iobuf);
        if (cr < 0) {
            snprintf(err, err_sz, "Queue copy failed (%d, errno %d)", cr, errno);
            return -1;
        }
        if (rename(tmp_path, final_path) < 0) {
            unlink(tmp_path);
            snprintf(err, err_sz, "Cannot finalize queue archive (errno %d)", errno);
            return -1;
        }
    }
    if (ls_queue_insert_row(id, filename, m, hex, size) < 0) {
        /* File is complete and stays; startup recovery will adopt it. */
        snprintf(err, err_sz, "Archive queued but metadata write failed");
        ls_event("QUEUE_METADATA_ERROR", m->title_id, "id=%s", id);
        return -1;
    }
    if (out_id) snprintf(out_id, out_id_sz, "%s", id);
    ls_event("BACKUP_QUEUED", m->title_id, "id=%s save=%s size=%lld sha256=%.16s reason=%s",
             id, m->save_name, (long long)size, hex, m->reason);
    ls_wait_bump("new");
    return 0;
}

static void ls_queue_set_cloud_needed(const char *id) {
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "UPDATE queue SET cloud_needed = 1 WHERE id = ?",
                               -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, id);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
}

/* Look up a ready queue row. Returns 1 if found. */
typedef struct {
    char id[200];
    char filename[220];
    char title_id[32];
    char save_name[256];
    char user_id[16];
    char sha256[65];
    char reason[32];
    char state[16];
    int64_t size, created_at, lan_ack_at, cloud_ack_at, gen_id, vault_ack_at;
    int cloud_needed, pc_attempts;
} ls_queue_row_t;

static void ls_col_text(sqlite3_stmt *st, int col, char *out, size_t out_sz) {
    const unsigned char *t = sqlite3_column_text(st, col);
    snprintf(out, out_sz, "%s", t ? (const char *)t : "");
}

#define LS_QUEUE_COLS "id, filename, title_id, save_name, user_id, sha256, reason, state," \
                      " size, created_at, lan_ack_at, cloud_ack_at, gen_id, cloud_needed, pc_attempts, vault_ack_at"

static void ls_queue_row_from(sqlite3_stmt *st, ls_queue_row_t *r) {
    memset(r, 0, sizeof(*r));
    ls_col_text(st, 0, r->id, sizeof(r->id));
    ls_col_text(st, 1, r->filename, sizeof(r->filename));
    ls_col_text(st, 2, r->title_id, sizeof(r->title_id));
    ls_col_text(st, 3, r->save_name, sizeof(r->save_name));
    ls_col_text(st, 4, r->user_id, sizeof(r->user_id));
    ls_col_text(st, 5, r->sha256, sizeof(r->sha256));
    ls_col_text(st, 6, r->reason, sizeof(r->reason));
    ls_col_text(st, 7, r->state, sizeof(r->state));
    r->size = sqlite3_column_int64(st, 8);
    r->created_at = sqlite3_column_int64(st, 9);
    r->lan_ack_at = sqlite3_column_int64(st, 10);
    r->cloud_ack_at = sqlite3_column_int64(st, 11);
    r->gen_id = sqlite3_column_int64(st, 12);
    r->cloud_needed = sqlite3_column_int(st, 13);
    r->pc_attempts = sqlite3_column_int(st, 14);
    r->vault_ack_at = sqlite3_column_int64(st, 15);
}

static int ls_queue_get(const char *id, ls_queue_row_t *r) {
    int found = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "SELECT " LS_QUEUE_COLS " FROM queue WHERE id = ?",
                               -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, id);
            if (sqlite3_step(st) == SQLITE_ROW) { ls_queue_row_from(st, r); found = 1; }
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    return found;
}

static void ls_queue_bump_attempts(const char *id) {
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "UPDATE queue SET pc_attempts = pc_attempts + 1 WHERE id = ?",
                               -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, id);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
}

/* dest: "pc" or "cloud". Returns 0 ok, -1 not found, -2 hash mismatch. */
static int ls_queue_ack(const char *id, const char *dest, const char *sha256) {
    ls_queue_row_t r;
    if (!ls_queue_get(id, &r)) return -1;
    if (sha256 && sha256[0] && strcasecmp(sha256, r.sha256) != 0) return -2;
    const char *sql = strcmp(dest, "cloud") == 0
        ? "UPDATE queue SET cloud_ack_at = ? WHERE id = ? AND cloud_ack_at = 0"
        : "UPDATE queue SET lan_ack_at = ? WHERE id = ? AND lan_ack_at = 0";
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
            ls_bind_text(st, 2, id);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    if (g_ls_db && g_ls_schema >= 3 && r.gen_id > 0) {
        sqlite3_stmt *st = NULL;
        const char *gsql = strcmp(dest, "cloud") == 0
            ? "UPDATE generations SET cloud_acked_at = ? WHERE gen_id = ? AND cloud_acked_at = 0"
            : "UPDATE generations SET pc_acked_at = ? WHERE gen_id = ? AND pc_acked_at = 0";
        if (sqlite3_prepare_v2(g_ls_db, gsql, -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
            sqlite3_bind_int64(st, 2, (sqlite3_int64)r.gen_id);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    if (strcmp(dest, "cloud") == 0) {
        ls_meta_set_int("last_cloud_ack", (int64_t)time(NULL));
        ls_event("CLOUD_ACK", r.title_id, "id=%s", id);
    } else {
        ls_meta_set_int("last_pc_ack", (int64_t)time(NULL));
        ls_event("PC_ACK", r.title_id, "id=%s save=%s", id, r.save_name);
    }
    return 0;
}

/* A stand-in PC client has the archive: recorded, but the PS5 still keeps it
 * until the primary client has it. 0 ok, -1 unknown, -2 SHA-256 mismatch. */
static int ls_queue_standin_ack(const char *id, const char *sha256, const char *cid) {
    ls_queue_row_t r;
    if (!ls_queue_get(id, &r)) return -1;
    if (!sha256 || strcasecmp(sha256, r.sha256) != 0) return -2;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "UPDATE queue SET standin_ack = ? WHERE id = ?", -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, cid);
            ls_bind_text(st, 2, id);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    ls_event("PC_STANDIN_ACK", r.title_id, "id=%s by %s (kept until the primary PC client has it)", id, cid);
    return 0;
}

/* The stand-in becomes the primary client: what it holds counts as delivered. */
static int ls_queue_promote_standin(const char *cid) {
    int n = 0;
    int64_t now = (int64_t)time(NULL);
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (g_ls_schema >= 3 && sqlite3_prepare_v2(g_ls_db,
                "UPDATE generations SET pc_acked_at = ? WHERE pc_acked_at = 0 AND gen_id IN"
                " (SELECT gen_id FROM queue WHERE standin_ack = ? AND lan_ack_at = 0 AND gen_id > 0)", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, now);
            ls_bind_text(st, 2, cid);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
        st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "UPDATE queue SET lan_ack_at = ? WHERE standin_ack = ? AND lan_ack_at = 0",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, now);
            ls_bind_text(st, 2, cid);
            if (sqlite3_step(st) == SQLITE_DONE) n = sqlite3_changes(g_ls_db);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    if (n) ls_meta_set_int("last_pc_ack", now);
    return n;
}

/* ── Retention ──────────────────────────────────────────────────── */
static int g_ls_queue_constrained = 0;

static void ls_queue_mark(const char *id, const char *state) {
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "UPDATE queue SET state = ? WHERE id = ?", -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, state);
            ls_bind_text(st, 2, id);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
}

/*
 * Delete archive files only when the PC has acknowledged them. Rows stay as
 * 'pruned' history. Order: acked and old (past keep_acked_hours), then acked
 * oldest-first while over the size cap (cloud-pending last). Unacknowledged
 * archives are never deleted; if they alone exceed the cap we warn.
 */
static void ls_queue_prune(const ls_config_t *cfg) {
    if (cfg->storage_ps5) return;          /* PS5-only mode: thinned by ls_ps5_housekeeping */
    int64_t cap = (int64_t)cfg->max_queue_mb * 1024 * 1024;
    int64_t keep_s = (int64_t)cfg->keep_acked_hours * 3600;
    int64_t now = (int64_t)time(NULL);
    int64_t total = 0, unacked = 0;

    enum { MAXC = 512 };
    char (*ids)[200] = malloc(sizeof(*ids) * MAXC);
    int64_t *sizes = malloc(sizeof(int64_t) * MAXC);
    int64_t *acked = malloc(sizeof(int64_t) * MAXC);
    int *cloud_pending = malloc(sizeof(int) * MAXC);
    if (!ids || !sizes || !acked || !cloud_pending) {
        free(ids); free(sizes); free(acked); free(cloud_pending);
        return;
    }
    int n = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db,
                /* "acknowledged" = every destination of the mode has it: the vault
                   (VPS only), the PC and the vault (both, PS5 uploads), else the PC */
                "SELECT id, size, CASE ? WHEN 1 THEN vault_ack_at"
                " WHEN 2 THEN (CASE WHEN lan_ack_at > 0 AND vault_ack_at > 0 THEN MAX(lan_ack_at, vault_ack_at) ELSE 0 END)"
                " ELSE lan_ack_at END, cloud_needed, cloud_ack_at FROM queue"
                " WHERE state = 'ready' ORDER BY created_at ASC", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int(st, 1, (cfg->storage_vps || cfg->vps_via_pc) ? (cfg->storage_pc ? 2 : 1) : 0);
            while (sqlite3_step(st) == SQLITE_ROW) {
                int64_t sz = sqlite3_column_int64(st, 1);
                int64_t la = sqlite3_column_int64(st, 2);
                total += sz;
                if (la == 0) { unacked += sz; continue; }
                if (n >= MAXC) continue;
                ls_col_text(st, 0, ids[n], sizeof(ids[n]));
                sizes[n] = sz;
                acked[n] = la;
                cloud_pending[n] = sqlite3_column_int(st, 3) && sqlite3_column_int64(st, 4) == 0;
                n++;
            }
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);

    /* pass 0: expired + no cloud pending; pass 1: over cap, no cloud pending;
       pass 2: over cap, cloud pending (LAN copy is safe; cloud retry is lost). */
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < n; i++) {
            if (!ids[i][0]) continue;
            int expired = (now - acked[i]) >= keep_s;
            int want = 0;
            if (pass == 0) want = expired && !cloud_pending[i];
            else if (pass == 1) want = total > cap && !cloud_pending[i];
            else want = total > cap;
            if (!want) continue;
            char path[MAX_PATH_LEN];
            snprintf(path, sizeof(path), "%s/%s.zip", LS_QUEUE_DIR, ids[i]);
            if (unlink(path) == 0 || errno == ENOENT) {
                ls_queue_mark(ids[i], "pruned");
                total -= sizes[i];
                if (pass == 2)
                    ls_event("QUEUE_PRUNED_CLOUD_PENDING", NULL, "id=%s (PC-acked; cloud copy not done)", ids[i]);
            }
            ids[i][0] = 0;
        }
    }
    free(ids); free(sizes); free(acked); free(cloud_pending);

    int constrained = unacked > cap;
    if (constrained && !g_ls_queue_constrained)
        ls_event("QUEUE_CAPACITY_WARNING", NULL,
                 "unacknowledged archives use %lld MB > cap %d MB; nothing deleted",
                 (long long)(unacked / (1024 * 1024)), cfg->max_queue_mb);
    g_ls_queue_constrained = constrained;
}

/* ── Startup recovery ───────────────────────────────────────────── */
static int ls_queue_has_filename(const char *filename) {
    int found = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "SELECT 1 FROM queue WHERE filename = ?", -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, filename);
            if (sqlite3_step(st) == SQLITE_ROW) found = 1;
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    return found;
}

/* Best-effort title id from "<stamp>_<TITLE>_<save>..." names. */
static void ls_guess_title(const char *name, char *out, size_t out_sz) {
    out[0] = 0;
    const char *p = strchr(name, '_');
    if (!p) return;
    p++;
    size_t n = 0;
    while (p[n] && p[n] != '_' && n + 1 < out_sz) n++;
    if (n == 9) { memcpy(out, p, n); out[n] = 0; }
}

static void ls_remove_stale_tmp(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (!ls_has_suffix(ent->d_name, ".tmp")) continue;
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (unlink(path) == 0)
            ls_event("STALE_TMP_REMOVED", NULL, "%s", path);
    }
    closedir(d);
}

static void ls_startup_recover(char *iobuf) {
    ls_event("STARTUP_RECOVERY_STARTED", NULL, "schema=%d pid=%d", g_ls_schema, (int)getpid());

    /* 1. Operations left running by a previous instance were interrupted. */
    int interrupted = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "SELECT id, kind, target FROM ops WHERE status = 'running'",
                               -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                interrupted++;
                logprintf("[LanSync] INTERRUPTED_OPERATION_FOUND op=%lld kind=%s target=%s\n",
                          (long long)sqlite3_column_int64(st, 0),
                          sqlite3_column_text(st, 1), sqlite3_column_text(st, 2));
            }
        }
        sqlite3_finalize(st);
        ls_exec_locked("UPDATE ops SET status = 'interrupted' WHERE status = 'running'");
    }
    pthread_mutex_unlock(&g_ls_mtx);
    if (interrupted)
        ls_event("INTERRUPTED_OPERATION_FOUND", NULL, "%d operation(s) marked interrupted", interrupted);

    /* 2. Half-written files are never valid. */
    ls_remove_stale_tmp(LS_QUEUE_DIR);
    ls_remove_stale_tmp(BACKUP_TMP_DIR);
    ls_remove_stale_tmp(CLOUD_QUEUE_DIR);

    /* 3. Finished staging ZIPs left behind by an interrupted job are complete
          (written .tmp -> fsync -> rename): adopt them instead of losing them. */
    DIR *d = opendir(BACKUP_TMP_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (!ls_has_suffix(ent->d_name, ".zip")) continue;
            char src[MAX_PATH_LEN], err[256] = {0};
            snprintf(src, sizeof(src), "%s/%s", BACKUP_TMP_DIR, ent->d_name);
            ls_queue_meta_t m;
            memset(&m, 0, sizeof(m));
            ls_guess_title(ent->d_name, m.title_id, sizeof(m.title_id));
            snprintf(m.save_name, sizeof(m.save_name), "recovered");
            snprintf(m.reason, sizeof(m.reason), "recovered_stage");
            if (ls_queue_add(src, 1, &m, NULL, 0, iobuf, err, sizeof(err)) == 0)
                ls_event("QUEUE_RECOVERED", m.title_id, "adopted orphan stage %s", ent->d_name);
            else
                ls_event("QUEUE_RECOVERY_FAILED", m.title_id, "%s: %s", ent->d_name, err);
        }
        closedir(d);
    }

    /* 4. Queue files without a row (metadata write lost): adopt in place. */
    int adopted = 0;
    d = opendir(LS_QUEUE_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (!ls_has_suffix(ent->d_name, ".zip")) continue;
            if (ls_queue_has_filename(ent->d_name)) continue;
            char path[MAX_PATH_LEN], hex[65];
            int64_t size = 0;
            snprintf(path, sizeof(path), "%s/%s", LS_QUEUE_DIR, ent->d_name);
            if (ls_sha256_file(path, hex, &size, iobuf) < 0) continue;
            char id[200];
            snprintf(id, sizeof(id), "%.*s", (int)(strlen(ent->d_name) - 4), ent->d_name);
            ls_queue_meta_t m;
            memset(&m, 0, sizeof(m));
            ls_guess_title(id, m.title_id, sizeof(m.title_id));
            snprintf(m.reason, sizeof(m.reason), "recovered_file");
            if (ls_queue_insert_row(id, ent->d_name, &m, hex, size) == 0) adopted++;
        }
        closedir(d);
    }
    if (adopted) ls_event("QUEUE_RECOVERED", NULL, "%d queue file(s) without metadata adopted", adopted);

    /* 5. Rows whose file disappeared are flagged, not silently dropped. */
    enum { MAXR = 1024 };
    char (*ids)[200] = malloc(sizeof(*ids) * MAXR);
    char (*files)[220] = malloc(sizeof(*files) * MAXR);
    int n = 0, missing = 0, ready = 0;
    if (ids && files) {
        pthread_mutex_lock(&g_ls_mtx);
        if (g_ls_db) {
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(g_ls_db, "SELECT id, filename FROM queue WHERE state = 'ready'",
                                   -1, &st, NULL) == SQLITE_OK) {
                while (sqlite3_step(st) == SQLITE_ROW && n < MAXR) {
                    ls_col_text(st, 0, ids[n], sizeof(ids[n]));
                    ls_col_text(st, 1, files[n], sizeof(files[n]));
                    n++;
                }
            }
            sqlite3_finalize(st);
        }
        pthread_mutex_unlock(&g_ls_mtx);
        for (int i = 0; i < n; i++) {
            char path[MAX_PATH_LEN];
            struct stat st;
            snprintf(path, sizeof(path), "%s/%s", LS_QUEUE_DIR, files[i]);
            if (stat(path, &st) < 0 || !S_ISREG(st.st_mode)) {
                ls_queue_mark(ids[i], "missing");
                ls_event("QUEUE_FILE_MISSING", NULL, "id=%s", ids[i]);
                missing++;
            } else {
                ready++;
            }
        }
    }
    free(ids);
    free(files);

    ls_meta_set_int("last_startup_recovery", (int64_t)time(NULL));
    ls_event("STARTUP_RECOVERY_COMPLETED", NULL, "queue_ready=%d missing=%d interrupted_ops=%d",
             ready, missing, interrupted);
}

static int g_ls_ready = 0;
static int g_ls_lock_fd = -1;

/*
 * Cross-process guard for the queue/state directory, independent of the HTTP
 * port: only the process holding this lock may recover, scan or package.
 * Returns 1 acquired, 0 held by another process, -1 locking unsupported
 * (then we proceed, as before).
 */
static int ls_acquire_instance_lock(void) {
    mkdir(LS_BASE_DIR, 0777);
    mkdir(LS_STATE_DIR, 0777);
    int fd = open(LS_STATE_DIR "/instance.lock", O_CREAT | O_RDWR, 0666);
    if (fd < 0) return -1;
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &fl) < 0) {
        int e = errno;
        if (e == EAGAIN || e == EACCES) { close(fd); return 0; }
        logprintf("[LanSync] instance lock unsupported (errno %d); continuing\n", e);
        g_ls_lock_fd = fd;
        return -1;
    }
    char pid[32];
    int n = snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
    if (ftruncate(fd, 0) == 0) { ssize_t w = pwrite(fd, pid, (size_t)n, 0); (void)w; }
    g_ls_lock_fd = fd;   /* kept open for the life of the process */
    return 1;
}

/* Returns 0 ready, -1 DB unavailable, -2 another process owns the queue. */
static int ls_init(void) {
    if (ls_acquire_instance_lock() == 0) {
        logprintf("[LanSync] another Shallot process owns %s; LAN sync disabled here\n", LS_STATE_DIR);
        return -2;
    }
    pthread_mutex_lock(&g_ls_mtx);
    int ok = ls_open_db() == 0;
    pthread_mutex_unlock(&g_ls_mtx);
    if (!ok) {
        logprintf("[LanSync] state DB unavailable; LAN backup queue disabled\n");
        return -1;
    }
    g_ls_ready = 1;
    char *iobuf = malloc(BUF_SIZE);
    if (iobuf) {
        ls_startup_recover(iobuf);
        free(iobuf);
    }
    ls_config_t cfg;
    ls_load_config(&cfg);
    ls_queue_prune(&cfg);
    return 0;
}
