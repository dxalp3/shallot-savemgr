/*
 * LAN save sync — save enumeration, cheap fingerprints and the save
 * stability detector. Included from main.c right after lansync_core.inc.c.
 *
 * Nothing here mounts or writes a save. Enumeration reads directory entries
 * and stat() data; the optional savedata.db lookup opens the system DB
 * read-only.
 */

#ifndef LS_USER_HOME
#define LS_USER_HOME   "/user/home"        /* overridden by host tests */
#endif
#ifndef LS_SYSTEM_DATA
#define LS_SYSTEM_DATA "/system_data"      /* overridden by host tests */
#endif

/* One save container on the console. */
typedef struct {
    char user_id[16];
    char title_id[32];
    char save_name[256];      /* file name inside savedata[_prospero]/<TID>/ */
    char path[MAX_PATH_LEN];
    int is_ps4;               /* lives under savedata/ (PS4 sdimg_ + .bin) */
} ls_save_ref_t;

/* Cheap metadata fingerprint of one save. */
typedef struct {
    int exists;
    int64_t size, blocks, mtime_ns, ctime_ns;
    int64_t bin_size, bin_mtime_ns;           /* PS4 sealed-key companion */
    char db_mtime[40];                        /* savedata.db row mtime, "" if unknown */
} ls_save_meta_t;

static int64_t ls_ts_ns(const struct timespec *ts) {
    return (int64_t)ts->tv_sec * 1000000000LL + (int64_t)ts->tv_nsec;
}

static void ls_bin_path_for(const ls_save_ref_t *s, char *out, size_t out_sz) {
    out[0] = 0;
    if (!s->is_ps4 || strncmp(s->save_name, "sdimg_", 6) != 0) return;
    char dir[MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s", s->path);
    char *sl = strrchr(dir, '/');
    if (sl) *(sl + 1) = 0;
    snprintf(out, out_sz, "%s%s.bin", dir, s->save_name + 6);
}

/* savedata.db stores dir_name without the PS4 "sdimg_" prefix. */
static const char *ls_db_dir_name(const ls_save_ref_t *s) {
    return (s->is_ps4 && strncmp(s->save_name, "sdimg_", 6) == 0) ? s->save_name + 6 : s->save_name;
}

/* Reads the savedata.db mtime column for a save; "" when unavailable. */
static void ls_db_mtime(sqlite3 *db, const ls_save_ref_t *s, char *out, size_t out_sz) {
    out[0] = 0;
    if (!db) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT mtime FROM savedata WHERE title_id = ? AND dir_name = ? LIMIT 1",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, s->title_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, ls_db_dir_name(s), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *t = sqlite3_column_text(st, 0);
            snprintf(out, out_sz, "%s", t ? (const char *)t : "");
        }
    }
    sqlite3_finalize(st);
}

static sqlite3 *ls_open_savedata_db(const char *user_id, int is_ps4) {
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s/%s/db/user/savedata.db", LS_SYSTEM_DATA,
             is_ps4 ? "savedata" : "savedata_prospero", user_id);
    struct stat st;
    if (stat(path, &st) < 0) return NULL;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 500);
    return db;
}

static void ls_save_meta_read(const ls_save_ref_t *s, sqlite3 *db, ls_save_meta_t *m) {
    memset(m, 0, sizeof(*m));
    struct stat st;
    if (stat(s->path, &st) < 0 || !S_ISREG(st.st_mode)) return;
    m->exists = 1;
    m->size = (int64_t)st.st_size;
    m->blocks = (int64_t)st.st_blocks;
    m->mtime_ns = ls_ts_ns(&st.st_mtim);
    m->ctime_ns = ls_ts_ns(&st.st_ctim);
    char bin[MAX_PATH_LEN];
    ls_bin_path_for(s, bin, sizeof(bin));
    if (bin[0] && stat(bin, &st) == 0) {
        m->bin_size = (int64_t)st.st_size;
        m->bin_mtime_ns = ls_ts_ns(&st.st_mtim);
    }
    ls_db_mtime(db, s, m->db_mtime, sizeof(m->db_mtime));
}

static int ls_save_meta_equal(const ls_save_meta_t *a, const ls_save_meta_t *b) {
    return a->exists == b->exists && a->size == b->size && a->blocks == b->blocks &&
           a->mtime_ns == b->mtime_ns && a->ctime_ns == b->ctime_ns &&
           a->bin_size == b->bin_size && a->bin_mtime_ns == b->bin_mtime_ns &&
           strcmp(a->db_mtime, b->db_mtime) == 0;
}

/* Compact, stable text form used for persistence and signatures. */
static void ls_save_meta_str(const ls_save_meta_t *m, char *out, size_t out_sz) {
    if (!m->exists) { snprintf(out, out_sz, "absent"); return; }
    snprintf(out, out_sz, "s=%lld;b=%lld;m=%lld;c=%lld;bs=%lld;bm=%lld;db=%s",
             (long long)m->size, (long long)m->blocks, (long long)m->mtime_ns,
             (long long)m->ctime_ns, (long long)m->bin_size, (long long)m->bin_mtime_ns,
             m->db_mtime);
}

/* ── Enumeration (no g_saves / g_mtx) ───────────────────────────── */
typedef struct {
    ls_save_ref_t *items;
    int count, cap;
} ls_save_list_t;

static void ls_save_list_free(ls_save_list_t *l) {
    free(l->items);
    memset(l, 0, sizeof(*l));
}

static ls_save_ref_t *ls_save_list_push(ls_save_list_t *l) {
    if (l->count >= l->cap) {
        int ncap = l->cap ? l->cap * 2 : 16;
        ls_save_ref_t *p = realloc(l->items, sizeof(*p) * (size_t)ncap);
        if (!p) return NULL;
        l->items = p;
        l->cap = ncap;
    }
    ls_save_ref_t *r = &l->items[l->count++];
    memset(r, 0, sizeof(*r));
    return r;
}

static int ls_save_ref_cmp(const void *a, const void *b) {
    const ls_save_ref_t *x = a, *y = b;
    int c = strcmp(x->user_id, y->user_id);
    if (c) return c;
    c = strcmp(x->title_id, y->title_id);
    if (c) return c;
    if (x->is_ps4 != y->is_ps4) return x->is_ps4 - y->is_ps4;
    return strcmp(x->save_name, y->save_name);
}

/* The scanner's only name filter (same rule as scan_title_dir()): skip dot
 * files and PS4 key companions "<name>.bin"; sdimg_* images are always saves,
 * even when named *.bin (Digimon Story Time Stranger); PS4 dirs only
 * contribute sdimg_* images. Shared with the savedata.db cross-check. */
static int ls_save_name_accepted(const char *name, int is_ps4) {
    if (name[0] == '.') return 0;
    int image = strncmp(name, "sdimg_", 6) == 0;
    if (!image && ls_has_suffix(name, ".bin")) return 0;
    if (is_ps4 && !image) return 0;
    return 1;
}

static void ls_enum_title_dir(ls_save_list_t *l, const char *dir, const char *user_id,
                              const char *title_id, int is_ps4) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (!ls_save_name_accepted(ent->d_name, is_ps4)) continue;
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) < 0 || !S_ISREG(st.st_mode)) continue;
        ls_save_ref_t *r = ls_save_list_push(l);
        if (!r) break;
        snprintf(r->user_id, sizeof(r->user_id), "%s", user_id);
        snprintf(r->title_id, sizeof(r->title_id), "%s", title_id);
        snprintf(r->save_name, sizeof(r->save_name), "%s", ent->d_name);
        snprintf(r->path, sizeof(r->path), "%s", path);
        r->is_ps4 = is_ps4;
    }
    closedir(d);
}

/* Enumerate saves of one title (title_id != NULL) or of every title. */
static void ls_enum_saves(ls_save_list_t *l, const char *title_id) {
    DIR *home = opendir(LS_USER_HOME);
    if (!home) return;
    struct dirent *ue;
    while ((ue = readdir(home))) {
        if (ue->d_name[0] == '.') continue;
        for (int ps4 = 0; ps4 < 2; ps4++) {
            char base[MAX_PATH_LEN];
            snprintf(base, sizeof(base), "%s/%s/%s", LS_USER_HOME, ue->d_name,
                     ps4 ? "savedata" : "savedata_prospero");
            if (title_id) {
                char dir[MAX_PATH_LEN];
                snprintf(dir, sizeof(dir), "%s/%s", base, title_id);
                ls_enum_title_dir(l, dir, ue->d_name, title_id, ps4);
                continue;
            }
            DIR *td = opendir(base);
            if (!td) continue;
            struct dirent *te;
            while ((te = readdir(td))) {
                if (te->d_name[0] == '.') continue;
                char dir[MAX_PATH_LEN];
                snprintf(dir, sizeof(dir), "%s/%s", base, te->d_name);
                struct stat st;
                if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
                    ls_enum_title_dir(l, dir, ue->d_name, te->d_name, ps4);
            }
            closedir(td);
        }
    }
    closedir(home);
    if (l->count > 1) qsort(l->items, (size_t)l->count, sizeof(*l->items), ls_save_ref_cmp);
}

/* ── Logical save objects ───────────────────────────────────────── */
/*
 * A game (title) holds one or more logical save objects; each object is a
 * primary image sdimg_<X> plus, when present, the system's backup copy
 * sdimg_sce_bu_<X> (PS4 key files ride along with their image). <X> is the
 * object id, derived generically from the file name ("Save04", "0000.bin",
 * "system"). Files stay the storage unit underneath (archives, generations,
 * sync state); objects are what backup/restore/reconcile reason about.
 */
enum { LS_ROLE_PRIMARY = 0, LS_ROLE_BACKUP = 1 };

/* Object id and role of a save file name. */
static int ls_object_of(const char *save_name, char *id, size_t id_sz) {
    const char *p = save_name;
    if (strncmp(p, "sdimg_", 6) == 0) p += 6;
    int role = LS_ROLE_PRIMARY;
    if (strncmp(p, "sce_bu_", 7) == 0 && p[7]) { p += 7; role = LS_ROLE_BACKUP; }
    snprintf(id, id_sz, "%s", p);
    return role;
}

static const char *ls_role_name(int role) { return role == LS_ROLE_BACKUP ? "sce_backup" : "primary"; }

/* File name of an object member, mirroring the naming of `like` (with or
 * without the sdimg_ prefix). */
static void ls_object_member_name(const char *like, const char *id, int role, char *out, size_t out_sz) {
    int sd = strncmp(like, "sdimg_", 6) == 0;
    snprintf(out, out_sz, "%s%s%s", sd ? "sdimg_" : "", role == LS_ROLE_BACKUP ? "sce_bu_" : "", id);
}

typedef struct {
    char user_id[16];
    char title_id[32];
    char id[256];
    int is_ps4;
    int member[2];            /* index into the save list per role, -1 if absent */
} ls_object_t;

typedef struct {
    ls_object_t *items;
    int count, cap;
} ls_object_list_t;

static void ls_object_list_free(ls_object_list_t *o) {
    free(o->items);
    memset(o, 0, sizeof(*o));
}

/* An orphan has a backup copy but no visible primary. */
static int ls_object_is_orphan(const ls_object_t *o) { return o->member[LS_ROLE_PRIMARY] < 0; }

/* Groups an enumerated save list into objects (same user, title, platform
 * and object id). Keeps the list order of first appearance. Returns 0 or -1. */
static int ls_group_objects(const ls_save_list_t *l, ls_object_list_t *o) {
    memset(o, 0, sizeof(*o));
    for (int i = 0; i < l->count; i++) {
        const ls_save_ref_t *s = &l->items[i];
        char id[256];
        int role = ls_object_of(s->save_name, id, sizeof(id));
        ls_object_t *hit = NULL;
        for (int k = 0; k < o->count && !hit; k++) {
            ls_object_t *x = &o->items[k];
            if (x->is_ps4 == s->is_ps4 && strcmp(x->id, id) == 0 && strcmp(x->title_id, s->title_id) == 0 &&
                strcmp(x->user_id, s->user_id) == 0 && x->member[role] < 0)
                hit = x;
        }
        if (!hit) {
            if (o->count >= o->cap) {
                int ncap = o->cap ? o->cap * 2 : 16;
                ls_object_t *p = realloc(o->items, sizeof(*p) * (size_t)ncap);
                if (!p) { ls_object_list_free(o); return -1; }
                o->items = p;
                o->cap = ncap;
            }
            hit = &o->items[o->count++];
            memset(hit, 0, sizeof(*hit));
            snprintf(hit->user_id, sizeof(hit->user_id), "%s", s->user_id);
            snprintf(hit->title_id, sizeof(hit->title_id), "%s", s->title_id);
            snprintf(hit->id, sizeof(hit->id), "%s", id);
            hit->is_ps4 = s->is_ps4;
            hit->member[0] = hit->member[1] = -1;
        }
        hit->member[role] = i;
    }
    return 0;
}

/* Per-object fingerprint: SHA-256 over each member's metadata (absent
 * members included), so a change in either the primary or its backup copy
 * changes the object's fingerprint and nothing else's. */
static void ls_object_fingerprint(const ls_save_list_t *l, const ls_save_meta_t *metas, const ls_object_t *o,
                                  char out[65]) {
    ls_sha256_t c;
    ls_sha256_init(&c);
    char line[600], ms[256];
    for (int role = 0; role < 2; role++) {
        int i = o->member[role];
        if (i < 0) snprintf(ms, sizeof(ms), "absent");
        else ls_save_meta_str(&metas[i], ms, sizeof(ms));
        int n = snprintf(line, sizeof(line), "%s|%s|%s\n", ls_role_name(role), i < 0 ? "-" : l->items[i].save_name, ms);
        ls_sha256_update(&c, line, (size_t)n);
    }
    ls_sha256_final_hex(&c, out);
}

/* JSON fields `"object_id":"X","role":"primary",` for a save file name. */
static const char *ls_object_json(const char *save_name, char *out, size_t out_sz) {
    char id[256], eid[520];
    int role = ls_object_of(save_name, id, sizeof(id));
    json_escape(id, eid, sizeof(eid));
    snprintf(out, out_sz, "\"object_id\":\"%s\",\"role\":\"%s\",", eid, ls_role_name(role));
    return out;
}

/* ── Cross-check against the PS5 save database ──────────────────── */
/*
 * The system's savedata.db lists every save of every title. A row whose
 * image sdimg_<dir_name> exists but is not accepted by the scanner means
 * Shallot cannot see (and therefore cannot protect) that save — the failure
 * mode found with Digimon's sdimg_*.bin names. Rows without any file are
 * counted separately (db_only) and are informational.
 */
typedef struct {
    char user_id[16];
    char title_id[32];
    char save_name[256];
    char path[MAX_PATH_LEN];
    char reason[64];
} ls_unscanned_t;

static int ls_find_unscanned(const char *title_id, ls_unscanned_t *out, int max, int *db_only) {
    int n = 0;
    if (db_only) *db_only = 0;
    DIR *home = opendir(LS_USER_HOME);
    if (!home) return 0;
    struct dirent *u;
    while ((u = readdir(home))) {
        if (u->d_name[0] == '.') continue;
        for (int ps4 = 0; ps4 < 2; ps4++) {
            sqlite3 *db = ls_open_savedata_db(u->d_name, ps4);
            if (!db) continue;
            sqlite3_stmt *st = NULL;
            const char *sql = title_id
                ? "SELECT title_id, dir_name FROM savedata WHERE title_id = ?"
                : "SELECT title_id, dir_name FROM savedata";
            if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
                if (title_id) sqlite3_bind_text(st, 1, title_id, -1, SQLITE_TRANSIENT);
                while (sqlite3_step(st) == SQLITE_ROW) {
                    const unsigned char *t = sqlite3_column_text(st, 0);
                    const unsigned char *dn = sqlite3_column_text(st, 1);
                    if (!t || !dn || !dn[0]) continue;
                    char name[256], path[MAX_PATH_LEN];
                    snprintf(name, sizeof(name), "sdimg_%s", (const char *)dn);
                    snprintf(path, sizeof(path), "%s/%s/%s/%s/%s", LS_USER_HOME, u->d_name,
                             ps4 ? "savedata" : "savedata_prospero", (const char *)t, name);
                    struct stat fst;
                    const char *why = NULL;
                    if (lstat(path, &fst) < 0) { if (db_only) (*db_only)++; continue; }
                    if (!S_ISREG(fst.st_mode)) why = "not a regular file";
                    else if (!ls_save_name_accepted(name, ps4)) why = "name rejected by scanner filter";
                    if (!why) continue;
                    if (n < max && out) {
                        ls_unscanned_t *x = &out[n];
                        snprintf(x->user_id, sizeof(x->user_id), "%s", u->d_name);
                        snprintf(x->title_id, sizeof(x->title_id), "%s", (const char *)t);
                        snprintf(x->save_name, sizeof(x->save_name), "%s", name);
                        snprintf(x->path, sizeof(x->path), "%s", path);
                        snprintf(x->reason, sizeof(x->reason), "%s", why);
                    }
                    n++;
                }
            }
            sqlite3_finalize(st);
            sqlite3_close(db);
        }
    }
    closedir(home);
    return n;
}

/* Read metadata for every save in the list (opens each user's savedata.db once). */
static void ls_read_metas(const ls_save_list_t *l, ls_save_meta_t *metas) {
    sqlite3 *db = NULL;
    char db_user[16] = {0};
    int db_ps4 = -1;
    for (int i = 0; i < l->count; i++) {
        const ls_save_ref_t *s = &l->items[i];
        if (db_ps4 != s->is_ps4 || strcmp(db_user, s->user_id) != 0) {
            if (db) sqlite3_close(db);
            db = ls_open_savedata_db(s->user_id, s->is_ps4);
            snprintf(db_user, sizeof(db_user), "%s", s->user_id);
            db_ps4 = s->is_ps4;
        }
        ls_save_meta_read(s, db, &metas[i]);
    }
    if (db) sqlite3_close(db);
}

/*
 * Title signature: SHA-256 over the sorted (user, save, metadata) list.
 * Captures content-metadata changes as well as saves appearing/disappearing.
 */
static int ls_title_signature(const char *title_id, char sig[65], int *save_count) {
    ls_save_list_t l = {0};
    ls_enum_saves(&l, title_id);
    ls_save_meta_t *metas = l.count ? calloc((size_t)l.count, sizeof(*metas)) : NULL;
    if (l.count && !metas) { ls_save_list_free(&l); return -1; }
    ls_read_metas(&l, metas);
    ls_sha256_t c;
    ls_sha256_init(&c);
    char line[MAX_PATH_LEN + 512], ms[256];
    for (int i = 0; i < l.count; i++) {
        ls_save_meta_str(&metas[i], ms, sizeof(ms));
        int n = snprintf(line, sizeof(line), "%s|%d|%s|%s\n", l.items[i].user_id,
                         l.items[i].is_ps4, l.items[i].save_name, ms);
        ls_sha256_update(&c, line, (size_t)n);
    }
    ls_sha256_final_hex(&c, sig);
    if (save_count) *save_count = l.count;
    free(metas);
    ls_save_list_free(&l);
    return 0;
}

/* ── Stability detector ─────────────────────────────────────────── */
enum { LS_STABLE = 0, LS_STABILITY_TIMEOUT = 1, LS_STABILITY_ABORTED = 2, LS_STABILITY_ERROR = 3 };

typedef struct {
    /* Produce the current signature; return 0 on success. */
    int (*sample)(void *ctx, char sig[65]);
    /* Return 1 to abort (e.g. a title started). May be NULL. */
    int (*should_abort)(void *ctx);
    void (*sleep_s)(void *ctx, int seconds);
    int64_t (*now_ms)(void *ctx);
    void *ctx;
} ls_stability_io_t;

typedef struct {
    int samples_taken;
    int changes_seen;
    int64_t waited_ms;
    char final_sig[65];
} ls_stability_result_t;

/*
 * Sample A; then every interval take a new sample. Each sample equal to the
 * previous one counts as a matching observation; any change resets the count.
 * Stable once `required` consecutive matches were seen (required=2 means
 * A==B==C). Gives up after timeout_s without claiming stability.
 */
static int ls_wait_stable(const ls_stability_io_t *io, const char *title_id,
                          int interval_s, int required, int timeout_s,
                          ls_stability_result_t *res) {
    memset(res, 0, sizeof(*res));
    char prev[65], cur[65];
    int64_t start = io->now_ms(io->ctx);
    if (io->sample(io->ctx, prev) != 0) return LS_STABILITY_ERROR;
    res->samples_taken = 1;
    int matches = 0;
    for (;;) {
        if (io->should_abort && io->should_abort(io->ctx)) {
            res->waited_ms = io->now_ms(io->ctx) - start;
            return LS_STABILITY_ABORTED;
        }
        io->sleep_s(io->ctx, interval_s);
        if (io->should_abort && io->should_abort(io->ctx)) {
            res->waited_ms = io->now_ms(io->ctx) - start;
            return LS_STABILITY_ABORTED;
        }
        if (io->sample(io->ctx, cur) != 0) return LS_STABILITY_ERROR;
        res->samples_taken++;
        res->waited_ms = io->now_ms(io->ctx) - start;
        if (strcmp(cur, prev) == 0) {
            matches++;
            if (matches >= required) {
                snprintf(res->final_sig, sizeof(res->final_sig), "%s", cur);
                ls_event("SAVE_STABLE", title_id, "after %lld ms, %d samples, %d change(s)",
                         (long long)res->waited_ms, res->samples_taken, res->changes_seen);
                return LS_STABLE;
            }
        } else {
            matches = 0;
            res->changes_seen++;
            snprintf(prev, sizeof(prev), "%s", cur);
            /* Log only the first change to avoid per-poll noise. */
            if (res->changes_seen == 1)
                ls_event("SAVE_STILL_CHANGING", title_id, "metadata changed after %lld ms",
                         (long long)res->waited_ms);
        }
        if (res->waited_ms >= (int64_t)timeout_s * 1000) {
            snprintf(res->final_sig, sizeof(res->final_sig), "%s", cur);
            ls_event("SAVE_STABILITY_TIMEOUT", title_id,
                     "still changing after %lld ms (%d samples, %d changes); not backed up as stable",
                     (long long)res->waited_ms, res->samples_taken, res->changes_seen);
            return LS_STABILITY_TIMEOUT;
        }
    }
}

/* Real-console I/O for ls_wait_stable over one title's saves. */
typedef struct {
    const char *title_id;
    int (*title_running)(const char *title_id);   /* abort when any title is running; may be NULL */
} ls_title_stability_ctx_t;

static int ls_io_title_sample(void *ctx, char sig[65]) {
    ls_title_stability_ctx_t *c = ctx;
    return ls_title_signature(c->title_id, sig, NULL);
}
static int ls_io_title_abort(void *ctx) {
    ls_title_stability_ctx_t *c = ctx;
    return c->title_running ? c->title_running(c->title_id) : 0;
}
static void ls_io_sleep(void *ctx, int s) { (void)ctx; sleep((unsigned)s); }
static int64_t ls_io_now_ms(void *ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
