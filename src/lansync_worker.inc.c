/*
 * LAN save sync — per-save protection state and the serialized backup worker
 * (stage C: automatic game-close backup).
 *
 * Included from main.c after auto_get_running_title(); uses cloud_* helpers,
 * zip_file_t, cloud_stage_zip_file and notify from main.c.
 *
 * Rules enforced here:
 *  - a save is "protected" only after its archive is in the persistent queue;
 *  - a metadata change whose container hash equals the last packaged hash is
 *    recorded without creating a duplicate archive;
 *  - if metadata changes while packaging, the archive is discarded and the
 *    save stays "changed" (retried later);
 *  - a title that is running again is never read.
 */

/* ── saves table (schema v2, created in lansync_core.inc.c) ───────── */
typedef struct {
    int found;
    char last_seen_meta[256];
    char last_packaged_meta[256];
    char last_packaged_sha[65];
    char last_queue_id[200];
    char state[16];
    int64_t last_packaged_at;
    int64_t current_gen_id;
} ls_save_row_t;

static void ls_saves_get(const ls_save_ref_t *s, ls_save_row_t *r) {
    memset(r, 0, sizeof(*r));
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db,
                "SELECT last_seen_meta, last_packaged_meta, last_packaged_sha, last_queue_id, state,"
                " last_packaged_at, current_gen_id FROM saves WHERE user_id = ? AND title_id = ? AND save_name = ?",
                -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, s->user_id);
            ls_bind_text(st, 2, s->title_id);
            ls_bind_text(st, 3, s->save_name);
            if (sqlite3_step(st) == SQLITE_ROW) {
                r->found = 1;
                ls_col_text(st, 0, r->last_seen_meta, sizeof(r->last_seen_meta));
                ls_col_text(st, 1, r->last_packaged_meta, sizeof(r->last_packaged_meta));
                ls_col_text(st, 2, r->last_packaged_sha, sizeof(r->last_packaged_sha));
                ls_col_text(st, 3, r->last_queue_id, sizeof(r->last_queue_id));
                ls_col_text(st, 4, r->state, sizeof(r->state));
                r->last_packaged_at = sqlite3_column_int64(st, 5);
                r->current_gen_id = sqlite3_column_int64(st, 6);
            }
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
}

/* Records an observation. Never touches the last_packaged_* columns. */
static void ls_saves_observe(const ls_save_ref_t *s, const char *meta, const char *state, const char *err) {
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db,
                "INSERT INTO saves(user_id, title_id, save_name, is_ps4, last_seen_meta, last_seen_at, state, last_error)"
                " VALUES(?,?,?,?,?,?,?,?)"
                " ON CONFLICT(user_id, title_id, save_name) DO UPDATE SET"
                "  last_seen_meta = excluded.last_seen_meta, last_seen_at = excluded.last_seen_at,"
                "  state = excluded.state, last_error = excluded.last_error",
                -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, s->user_id);
            ls_bind_text(st, 2, s->title_id);
            ls_bind_text(st, 3, s->save_name);
            sqlite3_bind_int(st, 4, s->is_ps4);
            ls_bind_text(st, 5, meta);
            sqlite3_bind_int64(st, 6, (sqlite3_int64)time(NULL));
            ls_bind_text(st, 7, state);
            ls_bind_text(st, 8, err);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
}

/* Marks the save protected by (meta, sha, queue_id). Called only after the
 * archive row exists in the queue (or the content was already protected). */
static void ls_saves_protected(const ls_save_ref_t *s, const char *meta, const char *sha,
                               const char *queue_id, int64_t gen_id) {
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db,
                "INSERT INTO saves(user_id, title_id, save_name, is_ps4, last_seen_meta, last_seen_at,"
                " last_packaged_meta, last_packaged_sha, last_packaged_at, last_queue_id, state, last_error,"
                " current_gen_id)"
                " VALUES(?,?,?,?,?,?,?,?,?,?,'protected','',?)"
                " ON CONFLICT(user_id, title_id, save_name) DO UPDATE SET"
                "  last_seen_meta = excluded.last_seen_meta, last_seen_at = excluded.last_seen_at,"
                "  last_packaged_meta = excluded.last_packaged_meta,"
                "  last_packaged_sha = excluded.last_packaged_sha,"
                "  last_packaged_at = CASE WHEN excluded.last_queue_id != '' THEN excluded.last_packaged_at"
                "                          ELSE saves.last_packaged_at END,"
                "  last_queue_id = CASE WHEN excluded.last_queue_id != '' THEN excluded.last_queue_id"
                "                       ELSE saves.last_queue_id END,"
                "  state = 'protected', last_error = '',"
                "  current_gen_id = CASE WHEN excluded.current_gen_id > 0 THEN excluded.current_gen_id"
                "                        ELSE saves.current_gen_id END",
                -1, &st, NULL) == SQLITE_OK) {
            int64_t now = (int64_t)time(NULL);
            ls_bind_text(st, 1, s->user_id);
            ls_bind_text(st, 2, s->title_id);
            ls_bind_text(st, 3, s->save_name);
            sqlite3_bind_int(st, 4, s->is_ps4);
            ls_bind_text(st, 5, meta);
            sqlite3_bind_int64(st, 6, (sqlite3_int64)now);
            ls_bind_text(st, 7, meta);
            ls_bind_text(st, 8, sha);
            sqlite3_bind_int64(st, 9, (sqlite3_int64)now);
            ls_bind_text(st, 10, queue_id ? queue_id : "");
            sqlite3_bind_int64(st, 11, (sqlite3_int64)gen_id);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
}

/* ── Generations (schema v3) ────────────────────────────────────── */
static int64_t ls_gen_query_int(const char *sql, const ls_save_ref_t *s, const char *text4, int64_t int4) {
    int64_t v = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, s->user_id);
            ls_bind_text(st, 2, s->title_id);
            ls_bind_text(st, 3, s->save_name);
            if (text4) ls_bind_text(st, 4, text4);
            else sqlite3_bind_int64(st, 4, (sqlite3_int64)int4);
            if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    return v;
}

/* Newest generation of this save whose container hash is sha (0 if none). */
static int64_t ls_gen_find(const ls_save_ref_t *s, const char *sha) {
    return ls_gen_query_int("SELECT COALESCE(MAX(gen_id), 0) FROM generations"
                            " WHERE user_id = ? AND title_id = ? AND save_name = ? AND sha256 = ?",
                            s, sha, 0);
}

static int64_t ls_gen_latest(const ls_save_ref_t *s) {
    return ls_gen_query_int("SELECT COALESCE(MAX(gen_id), 0) FROM generations"
                            " WHERE user_id = ? AND title_id = ? AND save_name = ? AND ? >= 0",
                            s, NULL, 0);
}

/* Newest generation after `gen` that the PC acknowledged (0 if none). */
static int64_t ls_gen_newer_pc_acked(const ls_save_ref_t *s, int64_t gen) {
    return ls_gen_query_int("SELECT COALESCE(MAX(gen_id), 0) FROM generations"
                            " WHERE user_id = ? AND title_id = ? AND save_name = ? AND gen_id > ?"
                            " AND pc_acked_at > 0", s, NULL, gen);
}

static int64_t ls_gen_insert(const ls_save_ref_t *s, const char *sha, int64_t size, const char *meta,
                             int64_t parent, const char *reason, const char *queue_id) {
    int64_t id = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db,
                "INSERT INTO generations(user_id, title_id, save_name, sha256, size, meta, parent_gen,"
                " created_at, reason, queue_id) VALUES(?,?,?,?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, s->user_id);
            ls_bind_text(st, 2, s->title_id);
            ls_bind_text(st, 3, s->save_name);
            ls_bind_text(st, 4, sha);
            sqlite3_bind_int64(st, 5, (sqlite3_int64)size);
            ls_bind_text(st, 6, meta);
            sqlite3_bind_int64(st, 7, (sqlite3_int64)parent);
            sqlite3_bind_int64(st, 8, (sqlite3_int64)time(NULL));
            ls_bind_text(st, 9, reason);
            ls_bind_text(st, 10, queue_id);
            if (sqlite3_step(st) == SQLITE_DONE) id = sqlite3_last_insert_rowid(g_ls_db);
        }
        sqlite3_finalize(st);
        if (id > 0 && queue_id && queue_id[0]) {
            st = NULL;
            if (sqlite3_prepare_v2(g_ls_db, "UPDATE queue SET gen_id = ? WHERE id = ?", -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
                ls_bind_text(st, 2, queue_id);
                sqlite3_step(st);
            }
            sqlite3_finalize(st);
        }
    }
    pthread_mutex_unlock(&g_ls_mtx);
    return id;
}

/* ── Regression collector (stage N) ────────────────────────────── */
/* While a protect run is active, ls_protect_save reports files whose
 * content went back to an older generation (and files that got new content)
 * to that run's collector; ls_recovery_process turns them into offers. One
 * collector at a time, owned by the thread that installed it (no TLS). */
typedef struct {
    char user_id[16];
    char save_name[256];
    char object_id[256];
    int64_t cur_gen, newest_gen;
    int changed;              /* 1: new content archived in this run */
} ls_regress_t;
typedef struct {
    ls_regress_t items[64];
    int n;
} ls_regress_list_t;
static ls_regress_list_t *g_ls_regress = NULL;
static pthread_t g_ls_regress_owner;
static pthread_mutex_t g_ls_regress_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Returns col if it became the active collector, NULL if another run owns one. */
static ls_regress_list_t *ls_regress_begin(ls_regress_list_t *col) {
    if (!col) return NULL;
    pthread_mutex_lock(&g_ls_regress_mtx);
    ls_regress_list_t *r = NULL;
    if (!g_ls_regress) {
        g_ls_regress = col;
        g_ls_regress_owner = pthread_self();
        r = col;
    }
    pthread_mutex_unlock(&g_ls_regress_mtx);
    return r;
}

static void ls_regress_end(ls_regress_list_t *col) {
    pthread_mutex_lock(&g_ls_regress_mtx);
    if (col && g_ls_regress == col) g_ls_regress = NULL;
    pthread_mutex_unlock(&g_ls_regress_mtx);
}

static void ls_regress_note(const ls_save_ref_t *s, int64_t cur, int64_t newest, int changed) {
    pthread_mutex_lock(&g_ls_regress_mtx);
    if (!g_ls_regress || !pthread_equal(g_ls_regress_owner, pthread_self()) || g_ls_regress->n >= 64) {
        pthread_mutex_unlock(&g_ls_regress_mtx);
        return;
    }
    ls_regress_t *r = &g_ls_regress->items[g_ls_regress->n++];
    memset(r, 0, sizeof(*r));
    snprintf(r->user_id, sizeof(r->user_id), "%s", s->user_id);
    snprintf(r->save_name, sizeof(r->save_name), "%s", s->save_name);
    ls_object_of(s->save_name, r->object_id, sizeof(r->object_id));
    r->cur_gen = cur;
    r->newest_gen = newest;
    r->changed = changed;
    pthread_mutex_unlock(&g_ls_regress_mtx);
}

static void ls_recovery_process(const char *title_id, const char *reason, const ls_regress_list_t *col, int64_t prev_snap);
static int64_t ls_title_latest_snap(const char *title_id) {
    int64_t v = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db && g_ls_schema >= 6)
        v = ls_db_int_locked("SELECT COALESCE(MAX(snap_id), 0) FROM snapshots WHERE title_id = ?", title_id, NULL, NULL, 0);
    pthread_mutex_unlock(&g_ls_mtx);
    return v;
}

/* ── Packaging one save ─────────────────────────────────────────── */
/* Optional hook so the worker can feed legacy local/USB/FTP/Drive targets
 * from the same queued archive. Set by main.c glue below. */
static void ls_distribute_legacy(const char *queue_id, const ls_save_ref_t *s, char *iobuf);

/* Returns 1 packaged, 0 nothing to do (unchanged / same content), -1 error,
 * -2 changed during packaging (discarded). */
static int ls_protect_save(const ls_save_ref_t *s, const ls_save_meta_t *meta, const char *reason,
                           char *iobuf, char *err, size_t err_sz) {
    char ms[256];
    ls_save_meta_str(meta, ms, sizeof(ms));
    ls_save_row_t row;
    ls_saves_get(s, &row);
    if (row.found && strcmp(row.state, "protected") == 0 && strcmp(row.last_packaged_meta, ms) == 0) {
        ls_saves_observe(s, ms, "protected", "");
        return 0;
    }

    /* Strong check: same container bytes as the last archive => no new archive. */
    char sha[65];
    int64_t sz = 0;
    if (ls_sha256_file(s->path, sha, &sz, iobuf) < 0) {
        snprintf(err, err_sz, "cannot read save container");
        ls_saves_observe(s, ms, "error", err);
        return -1;
    }
    if (row.found && row.last_packaged_sha[0] && strcmp(row.last_packaged_sha, sha) == 0) {
        ls_saves_protected(s, ms, sha, NULL, 0);
        ls_event("SAVE_UNCHANGED", s->title_id, "%s: metadata changed, content identical", s->save_name);
        return 0;
    }
    /* Content equals an older archived generation: already protected, no new
     * archive. If a newer generation exists this is a probable regression
     * (e.g. after a kernel panic); report it, never restore here. */
    int64_t known = ls_gen_find(s, sha);
    if (known > 0) {
        ls_saves_protected(s, ms, sha, NULL, known);
        int64_t newest = ls_gen_latest(s);
        if (newest > known && strcmp(reason, "restored") != 0) {      /* a deliberate restore is no regression */
            ls_regress_note(s, known, newest, 0);
            int64_t acked = ls_gen_newer_pc_acked(s, known);
            if (acked > 0)
                ls_event("KNOWN_REGRESSION_DETECTED", s->title_id,
                         "%s: current content = gen %lld; newer gen %lld is on the PC (recovery possible)",
                         s->save_name, (long long)known, (long long)acked);
            else
                ls_event("SAVE_MATCHES_OLDER_GENERATION", s->title_id,
                         "%s: current content = gen %lld; newest is gen %lld (not PC-acknowledged)",
                         s->save_name, (long long)known, (long long)newest);
        }
        return 0;
    }
    ls_event("SAVE_CHANGED", s->title_id, "%s", s->save_name);
    ls_event("BACKUP_STARTED", s->title_id, "%s reason=%s", s->save_name, reason);

    zip_file_t files[2];
    char names[2][512];
    char bin[MAX_PATH_LEN];
    int nfiles = 0;
    snprintf(names[0], sizeof(names[0]), "%s/%s", s->title_id, s->save_name);
    files[0].path = s->path;
    files[0].name = names[0];
    nfiles = 1;
    ls_bin_path_for(s, bin, sizeof(bin));
    struct stat bst;
    if (bin[0] && stat(bin, &bst) == 0 && S_ISREG(bst.st_mode)) {
        snprintf(names[1], sizeof(names[1]), "%s/%s.bin", s->title_id, s->save_name + 6);
        files[1].path = bin;
        files[1].name = names[1];
        nfiles = 2;
    }

    char stamp[32], safe[128], fname[256], stage[MAX_PATH_LEN];
    cloud_timestamp(stamp, sizeof(stamp));
    snprintf(safe, sizeof(safe), "%s", s->save_name);
    sanitize_filename(safe);
    /* pid + counter: two attempts can never share a staging file. */
    static unsigned int stage_seq = 0;
    snprintf(fname, sizeof(fname), "%s_%s_%s_p%d-%u_enc.zip", stamp, s->title_id, safe,
             (int)getpid(), ++stage_seq);
    sanitize_filename(fname);
    int64_t op = ls_op_begin("save_backup", fname);
    if (cloud_stage_zip_file(fname, files, nfiles, iobuf, stage, sizeof(stage), err, err_sz) < 0) {
        ls_op_end(op, "failed", err);
        ls_saves_observe(s, ms, "changed", err);
        return -1;
    }

    /* Optimistic consistency check: the save must not have moved while zipping. */
    ls_save_meta_t after;
    sqlite3 *db = ls_open_savedata_db(s->user_id, s->is_ps4);
    ls_save_meta_read(s, db, &after);
    if (db) sqlite3_close(db);
    char sha_after[65];
    if (!ls_save_meta_equal(meta, &after) ||
        ls_sha256_file(s->path, sha_after, NULL, iobuf) < 0 || strcmp(sha, sha_after) != 0) {
        unlink(stage);
        snprintf(err, err_sz, "save changed while packaging; discarded");
        ls_op_end(op, "failed", err);
        ls_saves_observe(s, ms, "changed", err);
        ls_event("SAVE_STILL_CHANGING", s->title_id, "%s changed during packaging", s->save_name);
        return -2;
    }
    ls_event("BACKUP_PACKAGED", s->title_id, "%s -> %s", s->save_name, fname);

    ls_queue_meta_t qm;
    memset(&qm, 0, sizeof(qm));
    snprintf(qm.title_id, sizeof(qm.title_id), "%s", s->title_id);
    snprintf(qm.save_name, sizeof(qm.save_name), "%s", s->save_name);
    snprintf(qm.user_id, sizeof(qm.user_id), "%s", s->user_id);
    snprintf(qm.reason, sizeof(qm.reason), "%s", reason);
    char qid[200] = {0};
    if (ls_queue_add(stage, 1, &qm, qid, sizeof(qid), iobuf, err, err_sz) < 0) {
        unlink(stage);
        ls_op_end(op, "failed", err);
        ls_saves_observe(s, ms, "changed", err);
        return -1;
    }
    int64_t parent = row.current_gen_id > 0 ? row.current_gen_id : ls_gen_latest(s);
    int64_t gen = ls_gen_insert(s, sha, sz, ms, parent, reason, qid);
    ls_saves_protected(s, ms, sha, qid, gen);
    ls_op_end(op, "ok", qid);
    ls_regress_note(s, gen, gen, 1);
    ls_distribute_legacy(qid, s, iobuf);
    return 1;
}

/* ── Snapshots (schema v6) ──────────────────────────────────────── */
/*
 * A snapshot records which archived generation of every file of a game
 * (kind "title") or of one save object (kind "object") belonged together.
 * It is only committed after every listed file is protected AND the files
 * did not change between the start and the end of the backup, so it
 * describes one coherent point in time. Unchanged state never produces a
 * second snapshot (the newest one is returned instead).
 */
typedef struct {
    char object_id[256];
    int role;
    char save_name[256];
    int is_ps4;
    int64_t gen_id;
    char sha[65];
    int64_t size;
    char queue_id[200];       /* archive of that generation ("" if none) */
} ls_snap_file_t;

static int ls_snap_file_cmp(const void *a, const void *b) {
    const ls_snap_file_t *x = a, *y = b;
    int c = strcmp(x->object_id, y->object_id);
    return c ? c : x->role - y->role;
}

/* Generation details (sha, size, archive) for one file entry. */
static void ls_snap_fill_gen_locked(ls_snap_file_t *f) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_ls_db, "SELECT sha256, COALESCE(size,0), COALESCE(queue_id,'') FROM generations WHERE gen_id = ?",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)f->gen_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            ls_col_text(st, 0, f->sha, sizeof(f->sha));
            f->size = sqlite3_column_int64(st, 1);
            ls_col_text(st, 2, f->queue_id, sizeof(f->queue_id));
        }
    }
    sqlite3_finalize(st);
}

/* Protected state of the listed files of one user (optionally one object).
 * Returns the number of files, or -1 (err set) if any file is unprotected. */
static int ls_snapshot_collect(const ls_save_list_t *l, const char *user_id, const char *object_id,
                               ls_snap_file_t **out, char *err, size_t err_sz) {
    *out = NULL;
    ls_snap_file_t *v = calloc((size_t)(l->count ? l->count : 1), sizeof(*v));
    if (!v) { snprintf(err, err_sz, "out of memory"); return -1; }
    int n = 0;
    for (int i = 0; i < l->count; i++) {
        const ls_save_ref_t *s = &l->items[i];
        if (strcmp(s->user_id, user_id) != 0) continue;
        ls_snap_file_t *f = &v[n];
        f->role = ls_object_of(s->save_name, f->object_id, sizeof(f->object_id));
        if (object_id && strcmp(f->object_id, object_id) != 0) continue;
        ls_save_row_t row;
        ls_saves_get(s, &row);
        if (!row.found || strcmp(row.state, "protected") != 0 || row.current_gen_id <= 0) {
            snprintf(err, err_sz, "%s is not protected (%s)", s->save_name, row.found ? row.state : "never backed up");
            free(v);
            return -1;
        }
        snprintf(f->save_name, sizeof(f->save_name), "%s", s->save_name);
        f->is_ps4 = s->is_ps4;
        f->gen_id = row.current_gen_id;
        pthread_mutex_lock(&g_ls_mtx);
        if (g_ls_db) ls_snap_fill_gen_locked(f);
        pthread_mutex_unlock(&g_ls_mtx);
        if (!f->sha[0]) { snprintf(err, err_sz, "%s: generation %lld unknown", s->save_name, (long long)f->gen_id); free(v); return -1; }
        n++;
    }
    if (n > 1) qsort(v, (size_t)n, sizeof(*v), ls_snap_file_cmp);
    *out = v;
    return n;
}

static void ls_snapshot_fp(const ls_snap_file_t *f, int n, char out[65]) {
    ls_sha256_t c;
    ls_sha256_init(&c);
    char line[700];
    for (int i = 0; i < n; i++) {
        int k = snprintf(line, sizeof(line), "%s|%d|%s|%s\n", f[i].object_id, f[i].role, f[i].save_name, f[i].sha);
        ls_sha256_update(&c, line, (size_t)k);
    }
    ls_sha256_final_hex(&c, out);
}

/* Files of a snapshot (sorted by object, role). Returns count (0 if none). */
static int ls_snapshot_files(int64_t snap_id, ls_snap_file_t **out) {
    *out = NULL;
    int n = 0, cap = 0;
    ls_snap_file_t *v = NULL;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db && g_ls_schema >= 6) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db,
                "SELECT f.object_id, f.role, f.save_name, f.is_ps4, f.gen_id, f.sha256, f.size, COALESCE(g.queue_id,'')"
                " FROM snapshot_files f LEFT JOIN generations g ON g.gen_id = f.gen_id WHERE f.snap_id = ?",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)snap_id);
            while (sqlite3_step(st) == SQLITE_ROW) {
                if (n >= cap) {
                    int nc = cap ? cap * 2 : 16;
                    ls_snap_file_t *p = realloc(v, sizeof(*p) * (size_t)nc);
                    if (!p) break;
                    v = p;
                    cap = nc;
                }
                ls_snap_file_t *f = &v[n++];
                memset(f, 0, sizeof(*f));
                char role[16];
                ls_col_text(st, 0, f->object_id, sizeof(f->object_id));
                ls_col_text(st, 1, role, sizeof(role));
                f->role = strcmp(role, "sce_backup") == 0 ? LS_ROLE_BACKUP : LS_ROLE_PRIMARY;
                ls_col_text(st, 2, f->save_name, sizeof(f->save_name));
                f->is_ps4 = sqlite3_column_int(st, 3);
                f->gen_id = sqlite3_column_int64(st, 4);
                ls_col_text(st, 5, f->sha, sizeof(f->sha));
                f->size = sqlite3_column_int64(st, 6);
                ls_col_text(st, 7, f->queue_id, sizeof(f->queue_id));
            }
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    if (n > 1) qsort(v, (size_t)n, sizeof(*v), ls_snap_file_cmp);
    *out = v;
    return n;
}

static int ls_count_objects(const ls_snap_file_t *f, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) if (i == 0 || strcmp(f[i].object_id, f[i - 1].object_id) != 0) c++;
    return c;
}

/* Commits (or re-uses) the snapshot of `files`. Returns its id, 0 on error. */
static int64_t ls_snapshot_store(const char *user_id, const char *title_id, const char *kind, const char *object_id,
                                 const char *reason, const ls_snap_file_t *files, int n, int *is_new) {
    *is_new = 0;
    char fp[65];
    ls_snapshot_fp(files, n, fp);
    int64_t last = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db && g_ls_schema >= 6) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "SELECT COALESCE(MAX(snap_id),0) FROM snapshots WHERE user_id = ? AND title_id = ?"
                               " AND kind = ? AND object_id = ?", -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, user_id);
            ls_bind_text(st, 2, title_id);
            ls_bind_text(st, 3, kind);
            ls_bind_text(st, 4, object_id);
            if (sqlite3_step(st) == SQLITE_ROW) last = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    if (last) {
        ls_snap_file_t *prev = NULL;
        int pn = ls_snapshot_files(last, &prev);
        char pfp[65];
        ls_snapshot_fp(prev, pn, pfp);
        free(prev);
        if (strcmp(pfp, fp) == 0) return last;
    }
    int64_t id = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db && g_ls_schema >= 6 && ls_exec_locked("BEGIN") == 0) {
        int ok = 0;
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "INSERT INTO snapshots(user_id, title_id, kind, object_id, created_at, reason,"
                               " objects, files, fingerprint) VALUES(?,?,?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, user_id);
            ls_bind_text(st, 2, title_id);
            ls_bind_text(st, 3, kind);
            ls_bind_text(st, 4, object_id);
            sqlite3_bind_int64(st, 5, (sqlite3_int64)time(NULL));
            ls_bind_text(st, 6, reason);
            sqlite3_bind_int(st, 7, ls_count_objects(files, n));
            sqlite3_bind_int(st, 8, n);
            ls_bind_text(st, 9, fp);
            if (sqlite3_step(st) == SQLITE_DONE) { id = sqlite3_last_insert_rowid(g_ls_db); ok = 1; }
        }
        sqlite3_finalize(st);
        for (int i = 0; ok && i < n; i++) {
            st = NULL;
            ok = 0;
            if (sqlite3_prepare_v2(g_ls_db, "INSERT INTO snapshot_files(snap_id, object_id, role, save_name, is_ps4, gen_id,"
                                   " sha256, size) VALUES(?,?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
                ls_bind_text(st, 2, files[i].object_id);
                ls_bind_text(st, 3, ls_role_name(files[i].role));
                ls_bind_text(st, 4, files[i].save_name);
                sqlite3_bind_int(st, 5, files[i].is_ps4);
                sqlite3_bind_int64(st, 6, (sqlite3_int64)files[i].gen_id);
                ls_bind_text(st, 7, files[i].sha);
                sqlite3_bind_int64(st, 8, (sqlite3_int64)files[i].size);
                ok = sqlite3_step(st) == SQLITE_DONE;
            }
            sqlite3_finalize(st);
        }
        if (ok) ls_exec_locked("COMMIT");
        else { ls_exec_locked("ROLLBACK"); id = 0; }
    }
    pthread_mutex_unlock(&g_ls_mtx);
    if (id) *is_new = 1;
    return id;
}

/* Takes the snapshot of every user's files in `l` (object_id NULL: whole game).
 * Returns the newest snapshot id involved, 0 if none could be taken. */
static int64_t ls_snapshot_take(const char *title_id, const ls_save_list_t *l, const char *object_id, const char *reason) {
    int64_t result = 0;
    for (int i = 0; i < l->count; i++) {
        int seen = 0;
        for (int k = 0; k < i && !seen; k++) seen = strcmp(l->items[k].user_id, l->items[i].user_id) == 0;
        if (seen) continue;
        const char *user = l->items[i].user_id;
        ls_snap_file_t *files = NULL;
        char err[300] = {0};
        int n = ls_snapshot_collect(l, user, object_id, &files, err, sizeof(err));
        if (n < 0) {
            ls_event("SNAPSHOT_SKIPPED", title_id, "%s%s: %s", object_id ? "save " : "game", object_id ? object_id : "", err);
            continue;
        }
        if (n == 0) { free(files); continue; }
        int is_new = 0;
        int64_t id = ls_snapshot_store(user, title_id, object_id ? "object" : "title", object_id ? object_id : "",
                                       reason, files, n, &is_new);
        if (is_new) {
            if (object_id)
                ls_event("SNAPSHOT_CREATED", title_id, "#%lld save %s: %d file(s), reason=%s", (long long)id, object_id, n, reason);
            else
                ls_event("SNAPSHOT_CREATED", title_id, "#%lld game: %d save object(s), %d file(s), reason=%s", (long long)id,
                         ls_count_objects(files, n), n, reason);
        }
        free(files);
        if (id > result) result = id;
    }
    return result;
}

/* ── Worker ─────────────────────────────────────────────────────── */
enum { LS_JOB_TITLE_CLOSED = 1, LS_JOB_TITLE_MANUAL = 2, LS_JOB_STARTUP = 3, LS_JOB_TITLE_STARTUP = 4,
       LS_JOB_TITLE_FOLLOWUP = 5, LS_JOB_IDLE_SWEEP = 6, LS_JOB_TITLE_IDLE = 7, LS_JOB_RESTORE = 8,
       LS_JOB_OBJECT = 9, LS_JOB_HOUSEKEEP = 10, LS_JOB_VAULT = 11 };

typedef struct ls_job {
    int kind;
    char title_id[32];
    int attempts;
    int64_t not_before_ms;
    int64_t since;        /* TITLE_CLOSED: epoch seconds the title started (0 = unknown) */
    char object_id[256];  /* OBJECT: the save object to back up */
    struct ls_job *next;
} ls_job_t;

static pthread_mutex_t g_ls_job_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ls_job_cv = PTHREAD_COND_INITIALIZER;
static ls_job_t *g_ls_jobs = NULL;
static char g_ls_worker_current[64] = {0};
static int g_ls_worker_started = 0;
static int64_t g_ls_last_backup_at = 0;
static char g_ls_last_error[256] = {0};

/*
 * Stage P1: power transition reported by the system probe
 * (lansync_system.inc.c). On 13.60 Shallot gets ~8 s after the signal before
 * shutdown/reboot ends it (rest: ~19 s, then frozen and resumed later).
 * While powering down: a game-close / manual / object backup already queued
 * runs at once (no stability wait); every other job is held; no save is
 * started after the budget; restores are refused.
 */
enum { LS_PWR_NONE = 0, LS_PWR_SIGNALLED = 1, LS_PWR_SHUTDOWN = 2, LS_PWR_REST = 3 };
static volatile int g_ls_power = LS_PWR_NONE;
static volatile int64_t g_ls_power_since = 0;
#define LS_PWR_BUDGET_S 4     /* last moment (s after the signal) to start packaging a save */

static int64_t ls_power_clock(void) {
#ifdef LS_HOST_TEST
    if (g_test_power_now) return g_test_power_now;
#endif
    return (int64_t)time(NULL);
}
static int ls_power_down(void) { int p = g_ls_power; return p == LS_PWR_SIGNALLED || p == LS_PWR_SHUTDOWN; }
static int ls_power_out_of_time(int64_t now) { return ls_power_down() && now - g_ls_power_since >= LS_PWR_BUDGET_S; }
static int ls_power_job_allowed(int kind, int64_t now) {
    if (!ls_power_down()) return 1;
    return (kind == LS_JOB_TITLE_CLOSED || kind == LS_JOB_TITLE_MANUAL || kind == LS_JOB_OBJECT) && !ls_power_out_of_time(now);
}
static const char *ls_power_name(int p) {
    return p == LS_PWR_SIGNALLED ? "signalled" : p == LS_PWR_SHUTDOWN ? "shutdown" : p == LS_PWR_REST ? "rest" : "none";
}
/* Phase change; powering down pulls queued backups forward, anything else wakes held jobs. */
static void ls_power_set(int phase, int64_t since) {
    pthread_mutex_lock(&g_ls_job_mtx);
    int was_down = ls_power_down();
    g_ls_power = phase;
    if (phase == LS_PWR_SIGNALLED) g_ls_power_since = since;
    if (ls_power_down() && !was_down) {
        int64_t now = ls_io_now_ms(NULL);
        for (ls_job_t *j = g_ls_jobs; j; j = j->next)
            if (j->kind == LS_JOB_TITLE_CLOSED || j->kind == LS_JOB_TITLE_MANUAL || j->kind == LS_JOB_OBJECT)
                j->not_before_ms = now;
    }
    pthread_cond_broadcast(&g_ls_job_cv);
    pthread_mutex_unlock(&g_ls_job_mtx);
}

static const char *ls_job_kind_name(int kind) {
    switch (kind) {
    case LS_JOB_TITLE_CLOSED: return "title_closed";
    case LS_JOB_TITLE_MANUAL: return "manual";
    case LS_JOB_STARTUP: return "startup_reconcile";
    case LS_JOB_TITLE_STARTUP: return "startup_protect";
    case LS_JOB_TITLE_FOLLOWUP: return "close_followup";
    case LS_JOB_IDLE_SWEEP: return "idle_sweep";
    case LS_JOB_TITLE_IDLE: return "idle_protect";
    case LS_JOB_RESTORE: return "restore";
    case LS_JOB_OBJECT: return "save_object";
    case LS_JOB_HOUSEKEEP: return "housekeeping";
    case LS_JOB_VAULT: return "vault_upload";
    default: return "?";
    }
}

/* Adds a job unless an identical (kind, title) job is already pending. */
static int ls_worker_enqueue_since(int kind, const char *title_id, int attempts, int delay_s, int64_t since);
static int ls_worker_enqueue(int kind, const char *title_id, int attempts, int delay_s) {
    return ls_worker_enqueue_since(kind, title_id, attempts, delay_s, 0);
}
static int ls_worker_enqueue_job(int kind, const char *title_id, const char *object_id, int attempts, int delay_s,
                                 int64_t since);
static int ls_worker_enqueue_since(int kind, const char *title_id, int attempts, int delay_s, int64_t since) {
    return ls_worker_enqueue_job(kind, title_id, "", attempts, delay_s, since);
}
static int ls_worker_enqueue_job(int kind, const char *title_id, const char *object_id, int attempts, int delay_s,
                                 int64_t since) {
    pthread_mutex_lock(&g_ls_job_mtx);
    for (ls_job_t *j = g_ls_jobs; j; j = j->next) {
        if (j->kind == kind && strcmp(j->title_id, title_id) == 0 && strcmp(j->object_id, object_id) == 0) {
            pthread_mutex_unlock(&g_ls_job_mtx);
            return 0;
        }
    }
    ls_job_t *job = calloc(1, sizeof(*job));
    if (!job) { pthread_mutex_unlock(&g_ls_job_mtx); return -1; }
    job->kind = kind;
    snprintf(job->title_id, sizeof(job->title_id), "%s", title_id);
    job->attempts = attempts;
    job->not_before_ms = ls_io_now_ms(NULL) + (int64_t)delay_s * 1000;
    job->since = since;
    snprintf(job->object_id, sizeof(job->object_id), "%s", object_id);
    ls_job_t **pp = &g_ls_jobs;
    while (*pp) pp = &(*pp)->next;
    *pp = job;
    pthread_cond_signal(&g_ls_job_cv);
    pthread_mutex_unlock(&g_ls_job_mtx);
    return 1;
}

static int ls_worker_pending(void) {
    int n = 0;
    pthread_mutex_lock(&g_ls_job_mtx);
    for (ls_job_t *j = g_ls_jobs; j; j = j->next) n++;
    pthread_mutex_unlock(&g_ls_job_mtx);
    return n;
}

/* 1 if the given title is the running Big App right now. */
static int ls_title_is_running(const char *title_id) {
    char running[32] = {0};
    return auto_get_running_title(running, sizeof(running), NULL) && strcmp(running, title_id) == 0;
}

/*
 * Protect every changed save of one title, then commit the game snapshot.
 * Returns the number of files left unprotected (a snapshot deferred because
 * the files moved during the backup counts as 1). *snap_out: the game
 * snapshot now describing the title (0 if none).
 */
static int ls_protect_title_ex(const char *title_id, const char *reason, char *iobuf, int64_t *snap_out) {
    if (snap_out) *snap_out = 0;
    ls_regress_list_t *alloc = calloc(1, sizeof(*alloc));
    ls_regress_list_t *col = ls_regress_begin(alloc);     /* NULL when nested in another run */
    if (!col) { free(alloc); alloc = NULL; }
    int64_t prev_snap = ls_title_latest_snap(title_id);
    char sig0[65] = {0};
    int have_sig = ls_title_signature(title_id, sig0, NULL) == 0;
    ls_save_list_t l = {0};
    ls_enum_saves(&l, title_id);
    ls_save_meta_t *metas = l.count ? calloc((size_t)l.count, sizeof(*metas)) : NULL;
    if (l.count && !metas) { ls_save_list_free(&l); ls_regress_end(col); free(alloc); return 1; }
    ls_read_metas(&l, metas);
    int packaged = 0, unchanged = 0, left = 0, objects = 0;
    char (*done_ids)[256] = l.count ? calloc((size_t)l.count, 256) : NULL;
    for (int i = 0; i < l.count; i++) {
        if (ls_title_is_running(title_id)) {
            ls_event("BACKUP_DEFERRED", title_id, "title started again; remaining saves deferred");
            left += l.count - i;
            break;
        }
        if (ls_power_out_of_time(ls_power_clock())) {
            int changed = 0;            /* only saves that differ from their last archive count */
            for (int k = i; k < l.count; k++) {
                char ms[256];
                ls_save_meta_str(&metas[k], ms, sizeof(ms));
                ls_save_row_t row;
                ls_saves_get(&l.items[k], &row);
                if (!row.found || strcmp(row.last_packaged_meta, ms) != 0) changed++;
            }
            if (changed) {
                ls_event("BACKUP_DEFERRED", title_id, "console is powering off; %d changed save(s) left for the next start",
                         changed);
                left += changed;
            }
            break;
        }
        char err[256] = {0};
        int r = ls_protect_save(&l.items[i], &metas[i], reason, iobuf, err, sizeof(err));
        if (r == 1) {
            packaged++;
            if (done_ids) {           /* distinct save objects with a new archive */
                char id[256];
                ls_object_of(l.items[i].save_name, id, sizeof(id));
                int dup = 0;
                for (int k = 0; k < objects && !dup; k++) dup = strcmp(done_ids[k], id) == 0;
                if (!dup) snprintf(done_ids[objects++], 256, "%s", id);
            }
        }
        else if (r == 0) unchanged++;
        else {
            left++;
            snprintf(g_ls_last_error, sizeof(g_ls_last_error), "%s/%s: %s",
                     title_id, l.items[i].save_name, err);
            ls_event("BACKUP_FAILED", title_id, "%s: %s", l.items[i].save_name, err);
        }
    }
    if (packaged) {
        g_ls_last_backup_at = (int64_t)time(NULL);
        ls_meta_set_int("last_backup", g_ls_last_backup_at);
        const char *gname = lookup_title(title_id);
        if (objects == 1 && done_ids)
            notify("Shallot SaveMgr: backed up save %s of %s", done_ids[0], gname && gname[0] ? gname : title_id);
        else
            notify("Shallot SaveMgr: backed up %d save(s) of %s", objects ? objects : packaged, gname && gname[0] ? gname : title_id);
    }
    if (packaged || left)
        ls_event("TITLE_BACKUP_DONE", title_id, "packaged=%d unchanged=%d left=%d", packaged, unchanged, left);
    else if (l.count)
        ls_event("SAVE_UNCHANGED", title_id, "no save changes (%d save(s))", l.count);
    /* Game snapshot: only if every file is protected and nothing moved
     * between the first and the last look (one coherent point in time). */
    if (left == 0 && l.count > 0) {
        char sig1[65] = {0};
        if (!have_sig || ls_title_signature(title_id, sig1, NULL) < 0 || strcmp(sig0, sig1) != 0) {
            ls_event("SNAPSHOT_DEFERRED", title_id, "saves changed while backing up; the game snapshot is taken on the retry");
            left = 1;
        } else {
            int64_t snap = ls_snapshot_take(title_id, &l, NULL, reason);
            if (snap_out) *snap_out = snap;
        }
    }
    free(done_ids);
    free(metas);
    ls_save_list_free(&l);
    ls_regress_end(col);
    if (col) ls_recovery_process(title_id, reason, col, prev_snap);
    free(alloc);
    return left;
}

static int ls_protect_title(const char *title_id, const char *reason, char *iobuf) {
    return ls_protect_title_ex(title_id, reason, iobuf, NULL);
}

/* Fingerprint of one object (all users) right now; "" if it has no files. */
static int ls_object_now_fp(const char *title_id, const char *object_id, char out[65]) {
    out[0] = 0;
    ls_save_list_t l = {0};
    ls_enum_saves(&l, title_id);
    ls_object_list_t o = {0};
    ls_save_meta_t *m = l.count ? calloc((size_t)l.count, sizeof(*m)) : NULL;
    int found = 0;
    if (m && ls_group_objects(&l, &o) == 0) {
        ls_read_metas(&l, m);
        ls_sha256_t c;
        ls_sha256_init(&c);
        for (int i = 0; i < o.count; i++) {
            if (strcmp(o.items[i].id, object_id) != 0) continue;
            char fp[65];
            ls_object_fingerprint(&l, m, &o.items[i], fp);
            ls_sha256_update(&c, o.items[i].user_id, strlen(o.items[i].user_id));
            ls_sha256_update(&c, fp, 64);
            found++;
        }
        if (found) ls_sha256_final_hex(&c, out);
    }
    free(m);
    ls_object_list_free(&o);
    ls_save_list_free(&l);
    return found;
}

/*
 * Back up one save object (primary + backup copy, every user) as a unit and
 * commit its object snapshot. Same rules as the whole game: unchanged files
 * are not archived again; the snapshot needs a stable object.
 * Returns files left unprotected (-1: object not found).
 */
static int ls_protect_object(const char *title_id, const char *object_id, const char *reason, char *iobuf,
                             int64_t *snap_out) {
    if (snap_out) *snap_out = 0;
    char fp0[65], fp1[65];
    if (!ls_object_now_fp(title_id, object_id, fp0)) return -1;
    ls_regress_list_t *alloc = calloc(1, sizeof(*alloc));
    ls_regress_list_t *col = ls_regress_begin(alloc);     /* NULL when nested in another run */
    if (!col) { free(alloc); alloc = NULL; }
    int64_t prev_snap = ls_title_latest_snap(title_id);
    ls_save_list_t all = {0}, l = {0};
    ls_enum_saves(&all, title_id);
    for (int i = 0; i < all.count; i++) {
        char id[256];
        ls_object_of(all.items[i].save_name, id, sizeof(id));
        if (strcmp(id, object_id) != 0) continue;
        ls_save_ref_t *r = ls_save_list_push(&l);
        if (r) *r = all.items[i];
    }
    ls_save_list_free(&all);
    ls_save_meta_t *metas = l.count ? calloc((size_t)l.count, sizeof(*metas)) : NULL;
    if (l.count && !metas) { ls_save_list_free(&l); ls_regress_end(col); free(alloc); return 1; }
    ls_read_metas(&l, metas);
    int packaged = 0, left = 0;
    for (int i = 0; i < l.count; i++) {
        if (ls_title_is_running(title_id) || ls_power_out_of_time(ls_power_clock())) { left += l.count - i; break; }
        char err[256] = {0};
        int r = ls_protect_save(&l.items[i], &metas[i], reason, iobuf, err, sizeof(err));
        if (r == 1) packaged++;
        else if (r < 0) {
            left++;
            ls_event("BACKUP_FAILED", title_id, "%s: %s", l.items[i].save_name, err);
        }
    }
    if (left == 0) {
        if (!ls_object_now_fp(title_id, object_id, fp1) || strcmp(fp0, fp1) != 0) {
            ls_event("SNAPSHOT_DEFERRED", title_id, "save %s changed while backing up", object_id);
            left = 1;
        } else {
            int64_t snap = ls_snapshot_take(title_id, &l, object_id, reason);
            if (snap_out) *snap_out = snap;
        }
    }
    if (packaged) {
        g_ls_last_backup_at = (int64_t)time(NULL);
        ls_meta_set_int("last_backup", g_ls_last_backup_at);
    }
    ls_event("OBJECT_BACKUP_DONE", title_id, "save %s: packaged=%d left=%d", object_id, packaged, left);
    free(metas);
    ls_save_list_free(&l);
    ls_regress_end(col);
    if (col) ls_recovery_process(title_id, reason, col, prev_snap);
    free(alloc);
    return left;
}

static void ls_startup_reconcile_job(void);
static void ls_idle_sweep_job(void);
static void ls_ps5_housekeeping(int limit);   /* lansync_blocks.inc.c */
static void (*g_ls_vault_job)(void);          /* set by lansync_vault.inc.c */
static void (*g_ls_health)(int64_t now);       /* problems worth a PS5 notification (lansync_vault.inc.c) */
static void ls_restore_run(char *iobuf);   /* lansync_restore.inc.c */
/* "manual" for Scan Changed Saves Now, "idle" for the timer (event detail only). */
static const char *g_ls_scan_trigger = "idle";

/* Enqueues a close job for every other title that has a save modified at or
 * after `since` (epoch seconds, 2 s slack). Returns the number queued. */
static int ls_queue_other_written_titles(const char *app_title, int64_t since) {
    ls_save_list_t l = {0};
    ls_enum_saves(&l, NULL);
    int queued = 0;
    int64_t since_ns = (since - 2) * 1000000000LL;
    for (int i = 0; i < l.count; i++) {
        const ls_save_ref_t *s = &l.items[i];
        if (strcmp(s->title_id, app_title) == 0) continue;
        struct stat st;
        if (stat(s->path, &st) < 0) continue;
        if (ls_ts_ns(&st.st_mtim) < since_ns) continue;
        if (ls_title_is_running(s->title_id)) continue;
        int r = ls_worker_enqueue(LS_JOB_TITLE_CLOSED, s->title_id, 0, 0);   /* deduplicated */
        if (r > 0) {
            queued++;
            ls_event("SAVE_TITLE_REDIRECT", app_title, "saves written under %s while %s ran; queued",
                     s->title_id, app_title);
        }
    }
    ls_save_list_free(&l);
    return queued;
}

static void ls_run_job(ls_job_t *job, char *iobuf) {
    ls_config_t cfg;
    ls_load_config(&cfg);
    if (!cfg.enabled) return;
    if (job->kind == LS_JOB_STARTUP) {
        ls_startup_reconcile_job();
        return;
    }
    if (job->kind == LS_JOB_RESTORE) {     /* checks the running title itself */
        ls_restore_run(iobuf);
        return;
    }
    if (job->kind == LS_JOB_VAULT) {
        if (g_ls_vault_job) g_ls_vault_job();
        return;
    }
    if (job->kind == LS_JOB_HOUSEKEEP) {
        ls_ps5_housekeeping(job->attempts > 0 ? job->attempts : 20);
        return;
    }
    if (job->kind == LS_JOB_IDLE_SWEEP) {
        g_ls_scan_trigger = strcmp(job->title_id, "manual") == 0 ? "manual" : "idle";
        ls_idle_sweep_job();
        return;
    }
    if (ls_title_is_running(job->title_id)) {
        ls_event("BACKUP_DEFERRED", job->title_id, "title is running; will retry after it closes");
        return;   /* the next GAME_EXIT enqueues it again */
    }
    if (job->kind == LS_JOB_OBJECT) {
        int left = ls_protect_object(job->title_id, job->object_id, "manual_object", iobuf, NULL);
        if (left < 0) ls_event("BACKUP_FAILED", job->title_id, "save %s not found", job->object_id);
        else if (left > 0 && job->attempts < 3)
            ls_worker_enqueue_job(LS_JOB_OBJECT, job->title_id, job->object_id, job->attempts + 1, 60 * (job->attempts + 1), 0);
        return;
    }
    /* Many games keep their saves under a different title ID than the app
     * (observed: PPSA24701 had none under its own ID). Every title with a
     * save written since the game started gets its own close job. */
    if (job->kind == LS_JOB_TITLE_CLOSED && job->since > 0 && job->attempts == 0)
        ls_queue_other_written_titles(job->title_id, job->since);

    /* Powering down: the game is closed and seconds count; package now. */
    int last_chance = ls_power_down();
    if (last_chance)
        ls_event("POWER_LAST_CHANCE", job->title_id, "console is powering off; backing up without the stability wait");
    if (cfg.stability_check && !last_chance) {
        ls_title_stability_ctx_t sc = { job->title_id, ls_title_is_running };
        ls_stability_io_t io = { ls_io_title_sample, ls_io_title_abort, ls_io_sleep, ls_io_now_ms, &sc };
        ls_stability_result_t res;
        int st = ls_wait_stable(&io, job->title_id, cfg.stability_interval_s, cfg.stability_samples,
                                cfg.stability_timeout_s, &res);
        if (st == LS_STABILITY_ABORTED) {
            ls_event("BACKUP_DEFERRED", job->title_id, "title started during stability wait");
            return;
        }
        if (st != LS_STABLE) {
            /* Not stable: record changed saves as 'unstable', retry with backoff. */
            ls_save_list_t l = {0};
            ls_enum_saves(&l, job->title_id);
            ls_save_meta_t *metas = l.count ? calloc((size_t)l.count, sizeof(*metas)) : NULL;
            if (metas) {
                ls_read_metas(&l, metas);
                for (int i = 0; i < l.count; i++) {
                    char ms[256];
                    ls_save_meta_t *m = &metas[i];
                    ls_save_meta_str(m, ms, sizeof(ms));
                    ls_save_row_t row;
                    ls_saves_get(&l.items[i], &row);
                    if (!row.found || strcmp(row.last_packaged_meta, ms) != 0)
                        ls_saves_observe(&l.items[i], ms, "unstable", "stability timeout");
                }
            }
            free(metas);
            ls_save_list_free(&l);
            if (job->attempts < 3) {
                int delay = 60 * (job->attempts + 1);
                ls_worker_enqueue(job->kind, job->title_id, job->attempts + 1, delay);
                ls_event("BACKUP_RETRY_SCHEDULED", job->title_id, "attempt %d in %ds", job->attempts + 2, delay);
            }
            return;
        }
    }

    int left = ls_protect_title(job->title_id,
                                job->kind == LS_JOB_TITLE_MANUAL ? "manual_changed" :
                                job->kind == LS_JOB_TITLE_STARTUP ? "startup_reconcile" :
                                job->kind == LS_JOB_TITLE_FOLLOWUP ? "close_followup" :
                                job->kind == LS_JOB_TITLE_IDLE ? "idle_sweep" : "game_close", iobuf);
    if (left > 0 && job->attempts < 3 && !last_chance) {
        int delay = 60 * (job->attempts + 1);
        ls_worker_enqueue(job->kind, job->title_id, job->attempts + 1, delay);
        ls_event("BACKUP_RETRY_SCHEDULED", job->title_id, "%d save(s) left; attempt %d in %ds",
                 left, job->attempts + 2, delay);
    }
    /* One delayed re-check per close catches late system writes such as the
     * sce_bu_* backup copies; it packages only what actually changed. */
    if (job->kind == LS_JOB_TITLE_CLOSED && job->attempts == 0 && cfg.followup_rescan_s > 0)
        ls_worker_enqueue(LS_JOB_TITLE_FOLLOWUP, job->title_id, 0, cfg.followup_rescan_s);
    if (cfg.storage_vps) ls_worker_enqueue(LS_JOB_VAULT, "", 0, 5);   /* VPS only: send the new archives */
    ls_config_t pcfg;
    ls_load_config(&pcfg);
    ls_queue_prune(&pcfg);
}

static void *ls_worker_thread(void *arg) {
    (void)arg;
    char *iobuf = malloc(BUF_SIZE);
    if (!iobuf) return NULL;
    for (;;) {
        pthread_mutex_lock(&g_ls_job_mtx);
        ls_job_t *job = NULL;
        while (!job) {
            int64_t now = ls_io_now_ms(NULL), soonest = -1;
            ls_job_t **pp = &g_ls_jobs;
            while (*pp) {
                int allowed = ls_power_job_allowed((*pp)->kind, ls_power_clock());
                if ((*pp)->not_before_ms <= now && allowed) { job = *pp; *pp = job->next; break; }
                int64_t at = allowed ? (*pp)->not_before_ms : now + 1000;    /* held: look again in 1 s */
                if (soonest < 0 || at < soonest) soonest = at;
                pp = &(*pp)->next;
            }
            if (job) break;
            if (soonest < 0) {
                pthread_cond_wait(&g_ls_job_cv, &g_ls_job_mtx);
            } else {
                /* Delayed retry pending: wake up periodically to check. */
                pthread_mutex_unlock(&g_ls_job_mtx);
                sleep(1);
                pthread_mutex_lock(&g_ls_job_mtx);
            }
        }
        snprintf(g_ls_worker_current, sizeof(g_ls_worker_current), "%s %s",
                 ls_job_kind_name(job->kind), job->title_id);
        pthread_mutex_unlock(&g_ls_job_mtx);

        ls_run_job(job, iobuf);

        pthread_mutex_lock(&g_ls_job_mtx);
        g_ls_worker_current[0] = 0;
        pthread_mutex_unlock(&g_ls_job_mtx);
        free(job);
    }
    return NULL;
}

static void ls_sys_probe_start(void);
static void ls_worker_start(void) {
    if (g_ls_worker_started || !g_ls_ready) return;
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    if (pthread_create(&tid, &attr, ls_worker_thread, NULL) == 0) {
        pthread_detach(tid);
        g_ls_worker_started = 1;
    }
    pthread_attr_destroy(&attr);
    ls_config_t cfg;
    ls_load_config(&cfg);
    /* Give the system a moment after (re)start before reading every save. */
    if (g_ls_worker_started && cfg.enabled && cfg.startup_reconcile)
        ls_worker_enqueue(LS_JOB_STARTUP, "", 0, 15);
    if (g_ls_worker_started && cfg.storage_vps) ls_worker_enqueue(LS_JOB_VAULT, "", 0, 60);
    if (g_ls_worker_started) ls_sys_probe_start();          /* stage O (lansync_system.inc.c) */
}

/* ── Startup reconciliation (stage F) ───────────────────────────── */
/*
 * Runs once after (re)start, as a worker job, before anything else touches
 * saves. Reconstructs state; never restores and never deletes:
 *  - saves of tracked titles whose metadata differs from what was last
 *    packaged (or that were left changed/unstable) => protect job per title
 *    (the protect path itself reports older-generation regressions);
 *  - new saves of tracked titles => protect job;
 *  - tracked saves that disappeared => state 'missing' + SAVE_MISSING;
 *  - saves of untracked titles => baseline observation only ('untracked'),
 *    unless startup_protect_untracked is set;
 *  - the running title (if any) is skipped; its exit triggers a normal job.
 */
typedef struct {
    int unchanged, changed, new_saves, missing, untracked, titles_queued, skipped_running, not_scanned;
} ls_reconcile_counts_t;

static int ls_title_tracked_locked(const char *title_id) {
    int tracked = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_ls_db,
            "SELECT 1 FROM saves WHERE title_id = ? AND last_packaged_sha IS NOT NULL"
            " AND last_packaged_sha != '' LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
        ls_bind_text(st, 1, title_id);
        tracked = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return tracked;
}

/* idle=0: startup reconciliation; idle=1: idle sweep / "Scan Changed Saves Now".
 * Same classification; different events, job kind and bookkeeping key. */
static void ls_scan_reconcile(int idle, ls_reconcile_counts_t *out) {
    ls_reconcile_counts_t c;
    memset(&c, 0, sizeof(c));
    if (out) memset(out, 0, sizeof(*out));     /* defined even on early return */
    ls_config_t cfg;
    ls_load_config(&cfg);
    char running[32] = {0};
    auto_get_running_title(running, sizeof(running), NULL);
    ls_event(idle ? "IDLE_SCAN_STARTED" : "STARTUP_RECONCILIATION_STARTED", NULL, "trigger=%s running=%s",
             idle ? g_ls_scan_trigger : "startup", running[0] ? running : "none");
    int64_t op = ls_op_begin(idle ? "idle_sweep" : "startup_reconcile", "");

    ls_save_list_t l = {0};
    ls_enum_saves(&l, NULL);
    ls_save_meta_t *metas = l.count ? calloc((size_t)l.count, sizeof(*metas)) : NULL;
    if (l.count && !metas) { ls_save_list_free(&l); ls_op_end(op, "failed", "out of memory"); return; }
    ls_read_metas(&l, metas);

    /* Titles needing a protect job; small linear set is fine. */
    char (*queue_titles)[32] = l.count ? calloc((size_t)l.count, 32) : NULL;
    int nq = 0;

    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) ls_exec_locked("BEGIN");
    for (int i = 0; g_ls_db && i < l.count; i++) {
        const ls_save_ref_t *s = &l.items[i];
        if (running[0] && strcmp(running, s->title_id) == 0) { c.skipped_running++; continue; }
        char ms[256];
        ls_save_meta_str(&metas[i], ms, sizeof(ms));

        char packaged[256] = {0}, state[16] = {0};
        int found = 0;
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db,
                "SELECT COALESCE(last_packaged_meta,''), COALESCE(state,'') FROM saves"
                " WHERE user_id = ? AND title_id = ? AND save_name = ?", -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, s->user_id);
            ls_bind_text(st, 2, s->title_id);
            ls_bind_text(st, 3, s->save_name);
            if (sqlite3_step(st) == SQLITE_ROW) {
                found = 1;
                ls_col_text(st, 0, packaged, sizeof(packaged));
                ls_col_text(st, 1, state, sizeof(state));
            }
        }
        sqlite3_finalize(st);

        int tracked = ls_title_tracked_locked(s->title_id);
        int needs = 0;
        const char *new_state = NULL;
        if (!tracked && !cfg.startup_protect_untracked) {
            c.untracked++;
            new_state = "untracked";
        } else if (!found || !packaged[0]) {
            c.new_saves++;
            needs = 1;
            new_state = "changed";
        } else if (strcmp(packaged, ms) != 0 || strcmp(state, "protected") != 0) {
            c.changed++;
            needs = 1;
            new_state = "changed";
        } else {
            c.unchanged++;
        }
        if (new_state) {
            st = NULL;
            if (sqlite3_prepare_v2(g_ls_db,
                    "INSERT INTO saves(user_id, title_id, save_name, is_ps4, last_seen_meta, last_seen_at, state)"
                    " VALUES(?,?,?,?,?,?,?) ON CONFLICT(user_id, title_id, save_name) DO UPDATE SET"
                    " last_seen_meta = excluded.last_seen_meta, last_seen_at = excluded.last_seen_at,"
                    " state = excluded.state", -1, &st, NULL) == SQLITE_OK) {
                ls_bind_text(st, 1, s->user_id);
                ls_bind_text(st, 2, s->title_id);
                ls_bind_text(st, 3, s->save_name);
                sqlite3_bind_int(st, 4, s->is_ps4);
                ls_bind_text(st, 5, ms);
                sqlite3_bind_int64(st, 6, (sqlite3_int64)time(NULL));
                ls_bind_text(st, 7, new_state);
                sqlite3_step(st);
            }
            sqlite3_finalize(st);
        }
        if (needs && queue_titles) {
            int dup = 0;
            for (int k = 0; k < nq; k++) if (strcmp(queue_titles[k], s->title_id) == 0) dup = 1;
            if (!dup) snprintf(queue_titles[nq++], 32, "%s", s->title_id);
        }
    }

    /* Tracked rows whose save no longer exists. */
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db,
                "SELECT user_id, title_id, save_name FROM saves"
                " WHERE state NOT IN ('missing', 'untracked')", -1, &st, NULL) == SQLITE_OK) {
            enum { MAXM = 256 };
            char (*miss)[3][256] = malloc(sizeof(*miss) * MAXM);
            int nm = 0;
            while (miss && sqlite3_step(st) == SQLITE_ROW) {
                ls_save_ref_t key;
                memset(&key, 0, sizeof(key));
                ls_col_text(st, 0, key.user_id, sizeof(key.user_id));
                ls_col_text(st, 1, key.title_id, sizeof(key.title_id));
                ls_col_text(st, 2, key.save_name, sizeof(key.save_name));
                if (running[0] && strcmp(running, key.title_id) == 0) continue;
                int present = 0;
                for (int i = 0; i < l.count && !present; i++)
                    present = strcmp(l.items[i].user_id, key.user_id) == 0 &&
                              strcmp(l.items[i].title_id, key.title_id) == 0 &&
                              strcmp(l.items[i].save_name, key.save_name) == 0;
                if (!present && nm < MAXM) {
                    snprintf(miss[nm][0], 256, "%s", key.user_id);
                    snprintf(miss[nm][1], 256, "%s", key.title_id);
                    snprintf(miss[nm][2], 256, "%s", key.save_name);
                    nm++;
                }
            }
            sqlite3_finalize(st);
            for (int i = 0; i < nm; i++) {
                st = NULL;
                if (sqlite3_prepare_v2(g_ls_db,
                        "UPDATE saves SET state = 'missing' WHERE user_id = ? AND title_id = ? AND save_name = ?",
                        -1, &st, NULL) == SQLITE_OK) {
                    ls_bind_text(st, 1, miss[i][0]);
                    ls_bind_text(st, 2, miss[i][1]);
                    ls_bind_text(st, 3, miss[i][2]);
                    sqlite3_step(st);
                }
                sqlite3_finalize(st);
            }
            c.missing = nm;
            if (g_ls_db) ls_exec_locked("COMMIT");
            pthread_mutex_unlock(&g_ls_mtx);
            for (int i = 0; i < nm; i++)
                ls_event("SAVE_MISSING", miss[i][1], "%s (user %s) is gone; archives are kept",
                         miss[i][2], miss[i][0]);
            free(miss);
        } else {
            ls_exec_locked("COMMIT");
            pthread_mutex_unlock(&g_ls_mtx);
        }
    } else {
        pthread_mutex_unlock(&g_ls_mtx);
    }

    for (int k = 0; k < nq; k++)
        if (ls_worker_enqueue(idle ? LS_JOB_TITLE_IDLE : LS_JOB_TITLE_STARTUP, queue_titles[k], 0, 0) > 0)
            c.titles_queued++;

    /* Saves the PS5 knows but the scanner skips: unprotectable until fixed. */
    {
        ls_unscanned_t un[32];
        c.not_scanned = ls_find_unscanned(NULL, un, 32, NULL);
        for (int i = 0; i < c.not_scanned && i < 32; i++)
            if (!idle)   /* logged once per start; idle sweeps only count */
                ls_event("SAVE_NOT_SCANNED", un[i].title_id, "%s: %s (%s)", un[i].save_name, un[i].reason, un[i].path);
    }

    free(queue_titles);
    free(metas);
    ls_save_list_free(&l);
    ls_meta_set_int(idle ? "last_idle_scan" : "last_startup_reconciliation", (int64_t)time(NULL));
    char detail[200];
    snprintf(detail, sizeof(detail),
             "unchanged=%d changed=%d new=%d missing=%d untracked=%d titles_queued=%d skipped_running=%d"
             " not_scanned=%d",
             c.unchanged, c.changed, c.new_saves, c.missing, c.untracked, c.titles_queued, c.skipped_running,
             c.not_scanned);
    ls_op_end(op, "ok", detail);
    ls_event(idle ? "IDLE_SCAN_COMPLETED" : "STARTUP_RECONCILIATION_COMPLETED", NULL, "%s", detail);
    if (idle)
        ls_event("IDLE_SCAN_CHANGED_COUNT", NULL, "%d changed, %d new save(s); %d title(s) queued",
                 c.changed, c.new_saves, c.titles_queued);
    if (out) *out = c;
}

/* Watcher hooks. */

static void ls_startup_reconcile(ls_reconcile_counts_t *out) { ls_scan_reconcile(0, out); }
static void ls_startup_reconcile_job(void) { ls_startup_reconcile(NULL); }
static void ls_idle_sweep_job(void) { ls_scan_reconcile(1, NULL); }

/* ── Idle sweep timer (stage K) ─────────────────────────────────── */
/*
 * Called by the watcher on every poll. After `idle_delay_min` without a
 * running title, one sweep is queued; while the console stays idle further
 * sweeps follow at most every 6 x idle_delay_min. Any title activity resets
 * the idle period. Returns 1 if a sweep was queued.
 */
static int64_t g_ls_last_activity = 0;
static int64_t g_ls_last_idle_sweep = 0;

static int ls_idle_tick(int title_running, int64_t now) {
    if (g_ls_health) g_ls_health(now);                     /* rate-limited inside */
    if (g_ls_last_activity == 0) g_ls_last_activity = now;   /* startup reconcile covers boot */
    if (title_running) { g_ls_last_activity = now; return 0; }
    ls_config_t cfg;
    ls_load_config(&cfg);
    if (!cfg.enabled || !cfg.idle_sweep || !g_ls_worker_started) return 0;
    int64_t delay = (int64_t)cfg.idle_delay_min * 60;
    if (now - g_ls_last_activity < delay) return 0;
    if (g_ls_last_idle_sweep >= g_ls_last_activity && now - g_ls_last_idle_sweep < delay * 6) return 0;
    if (ls_worker_enqueue(LS_JOB_IDLE_SWEEP, "", 0, 0) < 0) return 0;
    g_ls_last_idle_sweep = now;
    /* PS5-only mode: thin + dedup while the console is idle, at most hourly. */
    if (cfg.storage_ps5 && now - ls_meta_get_int("last_housekeeping", 0) >= 3600)
        ls_worker_enqueue(LS_JOB_HOUSEKEEP, "", 20, 30);
    return 1;
}
/* Start times of recently started titles, for the cross-title-ID scan. */
static struct { char title_id[32]; int64_t started; } g_ls_started[8];

static void ls_on_game_start(const char *title_id, int app_id) {
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (strcmp(g_ls_started[i].title_id, title_id) == 0) { slot = i; break; }
        if (g_ls_started[i].started < g_ls_started[slot].started) slot = i;
    }
    snprintf(g_ls_started[slot].title_id, sizeof(g_ls_started[slot].title_id), "%s", title_id);
    g_ls_started[slot].started = (int64_t)time(NULL);
    const char *name = lookup_title(title_id);   /* from app.db, loaded at startup */
    ls_event("GAME_START", title_id, "%s app 0x%x pid=%d", name ? name : "(unknown name)", app_id,
             (int)getpid());
}

/* Returns 1 if the LAN worker took ownership of the close event. */
static int ls_on_game_exit(const char *title_id) {
    const char *name = lookup_title(title_id);
    ls_event("GAME_EXIT", title_id, "%s pid=%d", name ? name : "(unknown name)", (int)getpid());
    ls_config_t cfg;
    ls_load_config(&cfg);
    if (!g_ls_worker_started || !cfg.enabled || !cfg.game_close_backup) return 0;
    int64_t since = 0;
    for (int i = 0; i < 8; i++)
        if (strcmp(g_ls_started[i].title_id, title_id) == 0) since = g_ls_started[i].started;
    ls_worker_enqueue_since(LS_JOB_TITLE_CLOSED, title_id, 0, 0, since);
    return 1;
}

static int ls_lan_watch_active(void) {
    ls_config_t cfg;
    ls_load_config(&cfg);
    return g_ls_worker_started && cfg.enabled && (cfg.game_close_backup || cfg.idle_sweep);
}

/* ── Legacy target distribution from the queued archive ─────────── */
static void ls_distribute_legacy(const char *queue_id, const ls_save_ref_t *s, char *iobuf) {
    cloud_config_t cfg;
    char err[256] = {0};
    if (cloud_load_config(&cfg, err, sizeof(err)) < 0 || !cfg.enabled) return;
    if (!cloud_title_is_watched(&cfg, s->title_id)) return;
    ls_queue_row_t r;
    if (!ls_queue_get(queue_id, &r)) return;
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", LS_QUEUE_DIR, r.filename);
    char out[MAX_PATH_LEN];
    if (cfg.targets.local.enabled) {
        err[0] = 0;
        if (cloud_copy_staged_zip_target(path, cfg.targets.local.path, s->title_id, s->user_id,
                                         r.filename, out, sizeof(out), err, sizeof(err)) < 0)
            ls_event("LEGACY_TARGET_FAILED", s->title_id, "local: %s", err);
    }
    if (cfg.targets.usb.enabled) {
        err[0] = 0;
        if (cloud_copy_staged_zip_target(path, cfg.targets.usb.path, s->title_id, s->user_id,
                                         r.filename, out, sizeof(out), err, sizeof(err)) < 0)
            ls_event("LEGACY_TARGET_FAILED", s->title_id, "usb: %s", err);
    }
    if (cfg.targets.ftp_enabled) {
        char remote_dir[768];
        err[0] = 0;
        cloud_remote_dir(&cfg, s->title_id, s->user_id, remote_dir, sizeof(remote_dir));
        if (cloud_upload_zip_file(&cfg, remote_dir, r.filename, path, iobuf, err, sizeof(err)) < 0)
            ls_event("LEGACY_TARGET_FAILED", s->title_id, "ftp: %s", err);
    }
    if (cfg.targets.drive_enabled && cfg.auto_backup.queue_for_browser_drive &&
        strcmp(cfg.drive.mode, "browser") == 0)
        ls_queue_set_cloud_needed(queue_id);
}
