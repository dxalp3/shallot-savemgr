/*
 * LAN save sync — manual reconciliation / diagnostics.
 * Included from main.c right after lansync_worker.inc.c.
 *
 * Stage G: Sanity Check (one title, metadata only).
 * Stage H: Full Reconcile Game (one title, hashes, background job) — see below.
 * Stages I/J: Quick Catalogue Check / Full Catalogue Reconciliation (+ Protect).
 *   - never packages, never hashes, never mounts, never writes a save;
 *   - only Shallot's own `reconcile` row is written (last result);
 *   - safe to run while other jobs run (read-only on saves and queue).
 *
 * Per-save status codes (cheapest evidence that supports them):
 *   HEALTHY                 metadata == last packaged, nothing pending
 *   CHANGED_SINCE_LAST_SCAN metadata differs from last packaged; whether the
 *                           content changed needs a hash (Back Up Changed
 *                           Saves or Full Reconcile decides)
 *   UNBACKED_CHANGE         Shallot already knows this version is unprotected
 *                           (changed / unstable / error / never packaged)
 *   KNOWN_REGRESSION        current content = an older generation while a
 *                           newer one is on the PC (recorded by the protect path)
 *   MISSING_SAVE            Shallot protected it before; it is gone now
 *   SAVE_NOT_SCANNED        the PS5 save database lists it, but the scanner
 *                           skips it (Shallot cannot protect it)
 *   QUEUE_INCONSISTENCY     a queue entry for this save lost its file
 *   MANIFEST_INCONSISTENCY  Shallot's records point at rows that do not exist
 *   UNTRACKED               title never protected by Shallot
 */

typedef enum {
    LS_ST_HEALTHY = 0,
    LS_ST_UNTRACKED,
    LS_ST_CHANGED_SINCE_LAST_SCAN,
    LS_ST_UNBACKED_CHANGE,
    LS_ST_NOT_SCANNED,
    LS_ST_QUEUE_INCONSISTENCY,
    LS_ST_MANIFEST_INCONSISTENCY,
    LS_ST_KNOWN_REGRESSION,
    LS_ST_MISSING_SAVE,
    LS_ST__COUNT
} ls_save_status_t;

/* Higher = more urgent; the title result is the most urgent save status. */
static const char *ls_status_name(ls_save_status_t s) {
    static const char *names[LS_ST__COUNT] = {
        "HEALTHY", "UNTRACKED", "CHANGED_SINCE_LAST_SCAN", "UNBACKED_CHANGE", "SAVE_NOT_SCANNED",
        "QUEUE_INCONSISTENCY", "MANIFEST_INCONSISTENCY", "KNOWN_REGRESSION", "MISSING_SAVE"
    };
    return (s >= 0 && s < LS_ST__COUNT) ? names[s] : "UNKNOWN";
}

static const char *ls_status_action(ls_save_status_t s) {
    switch (s) {
    case LS_ST_HEALTHY: return "No action needed";
    case LS_ST_UNTRACKED: return "Backup recommended: Shallot has never protected this title";
    case LS_ST_CHANGED_SINCE_LAST_SCAN:
        return "Backup recommended: metadata changed since the last protected version (Back Up Changed Saves checks content)";
    case LS_ST_UNBACKED_CHANGE: return "Backup recommended: current PS5 save is not protected";
    case LS_ST_NOT_SCANNED:
        return "Shallot cannot see this save (listed in the PS5 save database but skipped by the scanner); it is NOT protected - please report it";
    case LS_ST_QUEUE_INCONSISTENCY: return "Full scan recommended: a queued archive file is missing";
    case LS_ST_MANIFEST_INCONSISTENCY: return "Full scan recommended: Shallot's records are inconsistent";
    case LS_ST_KNOWN_REGRESSION: return "Recovery available: the PC holds a newer generation of this save";
    case LS_ST_MISSING_SAVE: return "Save missing on PS5: its archives are kept; inspect history";
    default: return "Full scan recommended";
    }
}

typedef struct {
    char user_id[16];
    char save_name[256];
    ls_save_status_t status;
    char detail[200];
    int pending_pc;          /* current generation not yet acknowledged by the PC */
} ls_sanity_item_t;

typedef struct {
    char title_id[32];
    int running;
    int tracked;
    ls_save_status_t result;
    int counts[LS_ST__COUNT];
    int pending_pc;
    int nitems;
    ls_sanity_item_t *items;
    char signature[65];
} ls_sanity_report_t;

static void ls_sanity_free(ls_sanity_report_t *r) {
    free(r->items);
    r->items = NULL;
    r->nitems = 0;
}

static ls_sanity_item_t *ls_sanity_add(ls_sanity_report_t *r, int *cap) {
    if (r->nitems >= *cap) {
        int ncap = *cap ? *cap * 2 : 16;
        ls_sanity_item_t *p = realloc(r->items, sizeof(*p) * (size_t)ncap);
        if (!p) return NULL;
        r->items = p;
        *cap = ncap;
    }
    ls_sanity_item_t *it = &r->items[r->nitems++];
    memset(it, 0, sizeof(*it));
    return it;
}


/* Classify one save that exists on disk. Called with g_ls_mtx held. */
static void ls_sanity_classify_locked(const ls_save_ref_t *s, const char *meta, int tracked,
                                      ls_sanity_item_t *it) {
    char packaged[256] = {0}, state[16] = {0}, last_qid[220] = {0};
    int64_t cur_gen = 0;
    int found = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_ls_db,
            "SELECT COALESCE(last_packaged_meta,''), COALESCE(state,''), COALESCE(current_gen_id,0),"
            " COALESCE(last_queue_id,'') FROM saves WHERE user_id = ? AND title_id = ? AND save_name = ?",
            -1, &st, NULL) == SQLITE_OK) {
        ls_bind_text(st, 1, s->user_id);
        ls_bind_text(st, 2, s->title_id);
        ls_bind_text(st, 3, s->save_name);
        if (sqlite3_step(st) == SQLITE_ROW) {
            found = 1;
            ls_col_text(st, 0, packaged, sizeof(packaged));
            ls_col_text(st, 1, state, sizeof(state));
            cur_gen = sqlite3_column_int64(st, 2);
            ls_col_text(st, 3, last_qid, sizeof(last_qid));
        }
    }
    sqlite3_finalize(st);

    if (!tracked) {
        it->status = LS_ST_UNTRACKED;
        snprintf(it->detail, sizeof(it->detail), "title never protected");
        return;
    }
    if (!found || !packaged[0]) {
        it->status = LS_ST_UNBACKED_CHANGE;
        snprintf(it->detail, sizeof(it->detail), "save has never been packaged");
        return;
    }

    /* Records must point at existing rows. */
    if (cur_gen > 0 && g_ls_schema >= 3) {
        sqlite3_stmt *g = NULL;
        int ok = 0;
        if (sqlite3_prepare_v2(g_ls_db, "SELECT 1 FROM generations WHERE gen_id = ? AND user_id = ?"
                               " AND title_id = ? AND save_name = ?", -1, &g, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(g, 1, (sqlite3_int64)cur_gen);
            ls_bind_text(g, 2, s->user_id);
            ls_bind_text(g, 3, s->title_id);
            ls_bind_text(g, 4, s->save_name);
            ok = sqlite3_step(g) == SQLITE_ROW;
        }
        sqlite3_finalize(g);
        if (!ok) {
            it->status = LS_ST_MANIFEST_INCONSISTENCY;
            snprintf(it->detail, sizeof(it->detail), "current generation %lld has no record", (long long)cur_gen);
            return;
        }
    }
    if (last_qid[0] &&
        !ls_db_int_locked("SELECT COUNT(*) FROM queue WHERE id = ?", last_qid, NULL, NULL, 0)) {
        it->status = LS_ST_MANIFEST_INCONSISTENCY;
        snprintf(it->detail, sizeof(it->detail), "last archive %.120s has no queue record", last_qid);
        return;
    }

    /* Queue file lost for this save's current archive? */
    if (last_qid[0]) {
        char fname[220] = {0}, qstate[16] = {0};
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "SELECT filename, state FROM queue WHERE id = ?", -1, &q, NULL) == SQLITE_OK) {
            ls_bind_text(q, 1, last_qid);
            if (sqlite3_step(q) == SQLITE_ROW) {
                ls_col_text(q, 0, fname, sizeof(fname));
                ls_col_text(q, 1, qstate, sizeof(qstate));
            }
        }
        sqlite3_finalize(q);
        int lost = strcmp(qstate, "missing") == 0;
        if (!lost && strcmp(qstate, "ready") == 0) {
            char path[MAX_PATH_LEN];
            struct stat fst;
            snprintf(path, sizeof(path), "%s/%s", LS_QUEUE_DIR, fname);
            lost = stat(path, &fst) < 0;
        }
        if (lost) {
            int64_t acked = cur_gen > 0
                ? ls_db_int_locked("SELECT COUNT(*) FROM generations WHERE pc_acked_at > 0 AND queue_id = ?",
                                   last_qid, NULL, NULL, 0)
                : 0;
            if (!acked) {
                it->status = LS_ST_QUEUE_INCONSISTENCY;
                snprintf(it->detail, sizeof(it->detail),
                         "archive %.120s is missing on the PS5 and was never acknowledged by the PC", last_qid);
                return;
            }
        }
    }

    /* Regression already established by the protect path (hash-based). */
    if (cur_gen > 0 && g_ls_schema >= 3) {
        int64_t newer_acked = ls_db_int_locked(
            "SELECT COALESCE(MAX(gen_id),0) FROM generations WHERE user_id = ? AND title_id = ?"
            " AND save_name = ? AND gen_id > ? AND pc_acked_at > 0",
            s->user_id, s->title_id, s->save_name, cur_gen);
        if (newer_acked > 0 && strcmp(packaged, meta) == 0) {
            it->status = LS_ST_KNOWN_REGRESSION;
            snprintf(it->detail, sizeof(it->detail), "current = gen %lld, PC holds newer gen %lld",
                     (long long)cur_gen, (long long)newer_acked);
            return;
        }
        sqlite3_stmt *pa = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "SELECT pc_acked_at FROM generations WHERE gen_id = ?",
                               -1, &pa, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(pa, 1, (sqlite3_int64)cur_gen);
            if (sqlite3_step(pa) == SQLITE_ROW) it->pending_pc = sqlite3_column_int64(pa, 0) == 0;
        }
        sqlite3_finalize(pa);
    }

    if (strcmp(state, "changed") == 0 || strcmp(state, "unstable") == 0 || strcmp(state, "error") == 0) {
        it->status = LS_ST_UNBACKED_CHANGE;
        snprintf(it->detail, sizeof(it->detail), "Shallot recorded state '%s'", state);
        return;
    }
    if (strcmp(packaged, meta) != 0) {
        it->status = LS_ST_CHANGED_SINCE_LAST_SCAN;
        snprintf(it->detail, sizeof(it->detail), "metadata differs from the last protected version");
        return;
    }
    it->status = LS_ST_HEALTHY;
    snprintf(it->detail, sizeof(it->detail), "%s", it->pending_pc ? "protected on PS5, PC download pending"
                                                                   : "protected");
}

/* Builds the report. Returns 0 on success. Never touches save files beyond stat(). */
static int ls_sanity_title(const char *title_id, ls_sanity_report_t *r) {
    memset(r, 0, sizeof(*r));
    snprintf(r->title_id, sizeof(r->title_id), "%s", title_id);
    r->running = ls_title_is_running(title_id);
    ls_event("RECONCILE_TITLE_SANITY_STARTED", title_id, "%s", r->running ? "title running" : "");

    ls_save_list_t l = {0};
    ls_enum_saves(&l, title_id);
    ls_save_meta_t *metas = l.count ? calloc((size_t)l.count, sizeof(*metas)) : NULL;
    if (l.count && !metas) { ls_save_list_free(&l); return -1; }
    ls_read_metas(&l, metas);
    ls_title_signature(title_id, r->signature, NULL);

    int cap = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (!g_ls_db) { pthread_mutex_unlock(&g_ls_mtx); free(metas); ls_save_list_free(&l); return -1; }
    r->tracked = ls_title_tracked_locked(title_id);
    for (int i = 0; i < l.count; i++) {
        ls_sanity_item_t *it = ls_sanity_add(r, &cap);
        if (!it) break;
        snprintf(it->user_id, sizeof(it->user_id), "%s", l.items[i].user_id);
        snprintf(it->save_name, sizeof(it->save_name), "%s", l.items[i].save_name);
        char ms[256];
        ls_save_meta_str(&metas[i], ms, sizeof(ms));
        ls_sanity_classify_locked(&l.items[i], ms, r->tracked, it);
    }
    /* Saves the PS5 knows but the scanner skips (invisible => unprotected). */
    {
        ls_unscanned_t un[32];
        int nun = ls_find_unscanned(title_id, un, 32, NULL);
        for (int i = 0; i < nun && i < 32; i++) {
            ls_sanity_item_t *it = ls_sanity_add(r, &cap);
            if (!it) break;
            snprintf(it->user_id, sizeof(it->user_id), "%s", un[i].user_id);
            snprintf(it->save_name, sizeof(it->save_name), "%s", un[i].save_name);
            it->status = LS_ST_NOT_SCANNED;
            snprintf(it->detail, sizeof(it->detail), "%s", un[i].reason);
        }
    }
    /* Saves Shallot protected before that are not on disk any more. */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_ls_db,
            "SELECT user_id, save_name FROM saves WHERE title_id = ? AND state != 'untracked'"
            " AND last_packaged_sha IS NOT NULL AND last_packaged_sha != ''", -1, &st, NULL) == SQLITE_OK) {
        ls_bind_text(st, 1, title_id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            char uid[16], save[256];
            ls_col_text(st, 0, uid, sizeof(uid));
            ls_col_text(st, 1, save, sizeof(save));
            int present = 0;
            for (int i = 0; i < l.count && !present; i++)
                present = strcmp(l.items[i].user_id, uid) == 0 && strcmp(l.items[i].save_name, save) == 0;
            if (present) continue;
            ls_sanity_item_t *it = ls_sanity_add(r, &cap);
            if (!it) break;
            snprintf(it->user_id, sizeof(it->user_id), "%s", uid);
            snprintf(it->save_name, sizeof(it->save_name), "%s", save);
            it->status = LS_ST_MISSING_SAVE;
            snprintf(it->detail, sizeof(it->detail), "protected before, not on the PS5 now; archives kept");
        }
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);

    r->result = LS_ST_HEALTHY;
    for (int i = 0; i < r->nitems; i++) {
        r->counts[r->items[i].status]++;
        if (r->items[i].pending_pc) r->pending_pc++;
        if (r->items[i].status > r->result) r->result = r->items[i].status;
    }
    free(metas);
    ls_save_list_free(&l);
    return 0;
}

static char *ls_sanity_json(const ls_sanity_report_t *r, int64_t checked_at) {
    size_t cap = 4096, pos = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    buf[0] = 0;
    ls_json_append(&buf, &cap, &pos,
        "{\"title_id\":\"%s\",\"check\":\"sanity\",\"workload\":\"quick\",\"checked_at\":%lld,"
        "\"running\":%s,\"tracked\":%s,\"result\":\"%s\",\"recommended_action\":\"%s\","
        "\"pending_pc\":%d,\"counts\":{",
        r->title_id, (long long)checked_at, r->running ? "true" : "false", r->tracked ? "true" : "false",
        ls_status_name(r->result),
        r->running ? "Title is running: results may change; check again after closing it"
                   : ls_status_action(r->result),
        r->pending_pc);
    int first = 1;
    for (int s = 0; s < LS_ST__COUNT; s++) {
        if (!r->counts[s]) continue;
        ls_json_append(&buf, &cap, &pos, "%s\"%s\":%d", first ? "" : ",", ls_status_name(s), r->counts[s]);
        first = 0;
    }
    ls_json_append(&buf, &cap, &pos, "},\"saves\":[");
    for (int i = 0; i < r->nitems; i++) {
        char es[520], ed[420], eo[600];
        json_escape(r->items[i].save_name, es, sizeof(es));
        json_escape(r->items[i].detail, ed, sizeof(ed));
        ls_json_append(&buf, &cap, &pos,
            "%s{\"user_id\":\"%s\",%s\"save_name\":\"%s\",\"status\":\"%s\",\"pending_pc\":%s,\"detail\":\"%s\"}",
            i ? "," : "", r->items[i].user_id, ls_object_json(r->items[i].save_name, eo, sizeof(eo)), es,
            ls_status_name(r->items[i].status),
            r->items[i].pending_pc ? "true" : "false", ed);
    }
    ls_json_append(&buf, &cap, &pos, "]}");
    return buf;
}

/* Persists the last result for the title (Shallot's own bookkeeping only). */
static void ls_reconcile_store(const char *title_id, const char *kind, const char *result,
                               const char *action, const char *signature, const char *report_json) {
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db && g_ls_schema >= 4) {
        const char *col = strcmp(kind, "full") == 0 ? "last_full_reconcile"
                        : strcmp(kind, "catalogue") == 0 ? "last_catalogue_scan" : "last_sanity_check";
        char sql[640];
        snprintf(sql, sizeof(sql),
            "INSERT INTO reconcile(title_id, %s, last_result, recommended_action, last_known_current_fp,"
            " last_report) VALUES(?,?,?,?,?,?) ON CONFLICT(title_id) DO UPDATE SET"
            " %s = excluded.%s, last_result = excluded.last_result,"
            " recommended_action = excluded.recommended_action,"
            " last_known_current_fp = excluded.last_known_current_fp, last_report = excluded.last_report",
            col, col, col);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, title_id);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)time(NULL));
            ls_bind_text(st, 3, result);
            ls_bind_text(st, 4, action);
            ls_bind_text(st, 5, signature);
            ls_bind_text(st, 6, report_json);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
}

/* Runs a sanity check and returns the JSON report (caller frees). */
static char *ls_sanity_title_json(const char *title_id) {
    ls_sanity_report_t r;
    if (ls_sanity_title(title_id, &r) < 0) return NULL;
    int64_t now = (int64_t)time(NULL);
    char *json = ls_sanity_json(&r, now);
    if (json)
        ls_reconcile_store(title_id, "sanity", ls_status_name(r.result),
                           r.running ? "Title is running" : ls_status_action(r.result), r.signature, json);
    ls_event("RECONCILE_TITLE_SANITY_COMPLETED", title_id, "result=%s saves=%d pending_pc=%d",
             ls_status_name(r.result), r.nitems, r.pending_pc);
    if (r.result != LS_ST_HEALTHY && r.result != LS_ST_UNTRACKED)
        ls_event("RECONCILE_ANOMALY_FOUND", title_id, "%s", ls_status_name(r.result));
    ls_sanity_free(&r);
    return json;
}

/* ── Background reconciliation jobs (stage H; reused by I/J) ─────── */
/*
 * One heavyweight reconciliation job at a time, in its own thread. Jobs are
 * read-only on saves; they may repair Shallot's own bookkeeping only where the
 * truth is established by a hash (each repair is listed in the report).
 * Cancellation is checked between saves/titles; already-written bookkeeping
 * stays consistent because every repair is a single-row update.
 */
typedef struct {
    int active;
    int cancel;
    char kind[24];              /* "title_full" (H), catalogue kinds later */
    char title_id[32];
    int protect;                /* "+ Protect Changed Saves" variant */
    int total, done;            /* saves (H) or titles (I/J) */
    char current[300];
    int64_t started_at, finished_at;
    char last_status[24];       /* running | completed | cancelled | failed */
    char last_summary[300];
} ls_rjob_t;

static pthread_mutex_t g_ls_rjob_mtx = PTHREAD_MUTEX_INITIALIZER;
static ls_rjob_t g_ls_rjob = {0};

static int ls_rjob_cancelled(void) {
    pthread_mutex_lock(&g_ls_rjob_mtx);
    int c = g_ls_rjob.cancel;
    pthread_mutex_unlock(&g_ls_rjob_mtx);
    return c;
}

static void ls_rjob_progress(int done, int total, const char *current) {
    pthread_mutex_lock(&g_ls_rjob_mtx);
    g_ls_rjob.done = done;
    g_ls_rjob.total = total;
    snprintf(g_ls_rjob.current, sizeof(g_ls_rjob.current), "%s", current ? current : "");
    pthread_mutex_unlock(&g_ls_rjob_mtx);
}

static void ls_rjob_finish(const char *status, const char *summary) {
    pthread_mutex_lock(&g_ls_rjob_mtx);
    g_ls_rjob.active = 0;
    g_ls_rjob.finished_at = (int64_t)time(NULL);
    snprintf(g_ls_rjob.last_status, sizeof(g_ls_rjob.last_status), "%s", status);
    snprintf(g_ls_rjob.last_summary, sizeof(g_ls_rjob.last_summary), "%s", summary ? summary : "");
    g_ls_rjob.current[0] = 0;
    pthread_mutex_unlock(&g_ls_rjob_mtx);
}

/* Claims the single job slot. Returns 0 on success, -1 if busy. */
static int ls_rjob_claim(const char *kind, const char *title_id, int protect) {
    pthread_mutex_lock(&g_ls_rjob_mtx);
    if (g_ls_rjob.active) { pthread_mutex_unlock(&g_ls_rjob_mtx); return -1; }
    memset(&g_ls_rjob, 0, sizeof(g_ls_rjob));
    g_ls_rjob.active = 1;
    g_ls_rjob.protect = protect;
    g_ls_rjob.started_at = (int64_t)time(NULL);
    snprintf(g_ls_rjob.kind, sizeof(g_ls_rjob.kind), "%s", kind);
    snprintf(g_ls_rjob.title_id, sizeof(g_ls_rjob.title_id), "%s", title_id ? title_id : "");
    snprintf(g_ls_rjob.last_status, sizeof(g_ls_rjob.last_status), "running");
    pthread_mutex_unlock(&g_ls_rjob_mtx);
    return 0;
}

static void ls_rjob_status_json(char **buf, size_t *cap, size_t *pos) {
    ls_rjob_t j;
    pthread_mutex_lock(&g_ls_rjob_mtx);
    j = g_ls_rjob;
    pthread_mutex_unlock(&g_ls_rjob_mtx);
    char ecur[620], esum[620];
    json_escape(j.current, ecur, sizeof(ecur));
    json_escape(j.last_summary, esum, sizeof(esum));
    ls_json_append(buf, cap, pos,
        "{\"active\":%s,\"kind\":\"%s\",\"title_id\":\"%s\",\"protect\":%s,\"done\":%d,\"total\":%d,"
        "\"current\":\"%s\",\"started_at\":%lld,\"finished_at\":%lld,\"status\":\"%s\",\"summary\":\"%s\"}",
        j.active ? "true" : "false", j.kind, j.title_id, j.protect ? "true" : "false", j.done, j.total,
        ecur, (long long)j.started_at, (long long)j.finished_at,
        j.last_status[0] ? j.last_status : "idle", esum);
}

/* ── Full Reconcile Game (Level 3) ──────────────────────────────── */
typedef struct {
    char user_id[16];
    char save_name[256];
    ls_save_status_t status;
    char sha[65];
    int64_t current_gen, matched_gen, newest_gen;
    int pending_pc;
    char detail[240];
    ls_sync_state_t sync;      /* vs. what the PC reported (stage L) */
    char sync_detail[220];
} ls_full_item_t;

typedef struct {
    char title_id[32];
    int running, tracked, cancelled;
    ls_save_status_t result;
    int counts[LS_ST__COUNT];
    int pending_pc;
    int nitems, cap;
    ls_full_item_t *items;
    int archives_checked, archives_corrupt, archives_missing;
    int pc_compared;
    int sync_counts[LS_SY__COUNT];
    int repairs;
    char repair_log[1024];
    char signature[65];
} ls_full_report_t;

static ls_full_item_t *ls_full_add(ls_full_report_t *r) {
    if (r->nitems >= r->cap) {
        int ncap = r->cap ? r->cap * 2 : 16;
        ls_full_item_t *p = realloc(r->items, sizeof(*p) * (size_t)ncap);
        if (!p) return NULL;
        r->items = p;
        r->cap = ncap;
    }
    ls_full_item_t *it = &r->items[r->nitems++];
    memset(it, 0, sizeof(*it));
    return it;
}

static void ls_full_repair(ls_full_report_t *r, const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    r->repairs++;
    size_t used = strlen(r->repair_log);
    snprintf(r->repair_log + used, sizeof(r->repair_log) - used, "%s%s", used ? "; " : "", line);
    ls_event("RECONCILE_REPAIR", r->title_id, "%s", line);
}

/* Archive integrity for every ready queue entry of the title. */
static void ls_full_check_archives(ls_full_report_t *r, char *iobuf) {
    enum { MAXQ = 512 };
    char (*ids)[200] = malloc(sizeof(*ids) * MAXQ);
    char (*files)[220] = malloc(sizeof(*files) * MAXQ);
    char (*shas)[65] = malloc(sizeof(*shas) * MAXQ);
    int n = 0;
    if (!ids || !files || !shas) { free(ids); free(files); free(shas); return; }
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *st = NULL;
    if (g_ls_db && sqlite3_prepare_v2(g_ls_db,
            "SELECT id, filename, sha256 FROM queue WHERE title_id = ? AND state = 'ready'",
            -1, &st, NULL) == SQLITE_OK) {
        ls_bind_text(st, 1, r->title_id);
        while (sqlite3_step(st) == SQLITE_ROW && n < MAXQ) {
            ls_col_text(st, 0, ids[n], sizeof(ids[n]));
            ls_col_text(st, 1, files[n], sizeof(files[n]));
            ls_col_text(st, 2, shas[n], sizeof(shas[n]));
            n++;
        }
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    for (int i = 0; i < n; i++) {
        if (ls_rjob_cancelled()) break;
        char path[MAX_PATH_LEN], hex[65];
        snprintf(path, sizeof(path), "%s/%s", LS_QUEUE_DIR, files[i]);
        r->archives_checked++;
        if (ls_sha256_file(path, hex, NULL, iobuf) < 0) {
            r->archives_missing++;
            ls_queue_mark(ids[i], "missing");
            ls_full_repair(r, "queue entry %s marked missing (file gone)", ids[i]);
            continue;
        }
        if (strcmp(hex, shas[i]) != 0) {
            r->archives_corrupt++;
            /* Never deleted: kept for inspection. */
            ls_event("QUEUE_ARCHIVE_CORRUPT", r->title_id, "%s sha256 %.16s != recorded %.16s",
                     ids[i], hex, shas[i]);
        }
    }
    free(ids); free(files); free(shas);
}

/* Classifies one on-disk save from its content hash. */
static void ls_full_classify(ls_full_report_t *r, const ls_save_ref_t *s, const char *meta,
                             const char *sha, ls_full_item_t *it) {
    snprintf(it->sha, sizeof(it->sha), "%s", sha);
    if (!r->tracked) {
        it->status = LS_ST_UNTRACKED;
        snprintf(it->detail, sizeof(it->detail), "title never protected");
        return;
    }
    ls_save_row_t row;
    ls_saves_get(s, &row);
    it->current_gen = row.current_gen_id;
    it->matched_gen = ls_gen_find(s, sha);
    it->newest_gen = ls_gen_latest(s);

    if (!it->matched_gen) {
        it->status = LS_ST_UNBACKED_CHANGE;
        snprintf(it->detail, sizeof(it->detail), "content matches no archived generation");
        return;
    }

    /* Is there a surviving copy of the matched generation (PS5 queue or PC)? */
    int64_t pc_acked = 0;
    char qid[220] = {0}, qstate[16] = {0};
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *st = NULL;
    if (g_ls_db && sqlite3_prepare_v2(g_ls_db,
            "SELECT g.pc_acked_at, COALESCE(g.queue_id,''), COALESCE(q.state,'')"
            " FROM generations g LEFT JOIN queue q ON q.id = g.queue_id WHERE g.gen_id = ?",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)it->matched_gen);
        if (sqlite3_step(st) == SQLITE_ROW) {
            pc_acked = sqlite3_column_int64(st, 0);
            ls_col_text(st, 1, qid, sizeof(qid));
            ls_col_text(st, 2, qstate, sizeof(qstate));
        }
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    it->pending_pc = pc_acked == 0;
    /* A 'ready' row is only a surviving copy if its file is really there. */
    if (strcmp(qstate, "ready") == 0) {
        ls_queue_row_t qr;
        char qpath[MAX_PATH_LEN];
        struct stat qst;
        if (!ls_queue_get(qid, &qr)) qstate[0] = 0;
        else {
            snprintf(qpath, sizeof(qpath), "%s/%s", LS_QUEUE_DIR, qr.filename);
            if (stat(qpath, &qst) < 0) snprintf(qstate, sizeof(qstate), "file missing");
        }
    }

    /* Bookkeeping repairs established by the hash. */
    if (row.current_gen_id != it->matched_gen || strcmp(row.last_packaged_meta, meta) != 0 ||
        strcmp(row.state, "protected") != 0) {
        ls_saves_protected(s, meta, sha, NULL, it->matched_gen);
        ls_full_repair(r, "%s: records updated to gen %lld (content verified by hash)",
                       s->save_name, (long long)it->matched_gen);
        it->current_gen = it->matched_gen;
    }

    if (!pc_acked && strcmp(qstate, "ready") != 0) {
        it->status = LS_ST_QUEUE_INCONSISTENCY;
        snprintf(it->detail, sizeof(it->detail),
                 "gen %lld has no surviving archive (not on PC, PS5 copy %s)",
                 (long long)it->matched_gen, qstate[0] ? qstate : "unknown");
        return;
    }
    if (it->newest_gen > it->matched_gen) {
        int64_t newer_pc = ls_gen_newer_pc_acked(s, it->matched_gen);
        it->status = LS_ST_KNOWN_REGRESSION;
        if (newer_pc)
            snprintf(it->detail, sizeof(it->detail), "content = gen %lld; PC holds newer gen %lld",
                     (long long)it->matched_gen, (long long)newer_pc);
        else
            snprintf(it->detail, sizeof(it->detail),
                     "content = gen %lld; newer gen %lld exists on the PS5 queue only",
                     (long long)it->matched_gen, (long long)it->newest_gen);
        return;
    }
    it->status = LS_ST_HEALTHY;
    snprintf(it->detail, sizeof(it->detail), "content = gen %lld%s", (long long)it->matched_gen,
             pc_acked ? ", on PC" : ", PC download pending");
}

static int ls_full_title(const char *title_id, ls_full_report_t *r, char *iobuf) {
    memset(r, 0, sizeof(*r));
    snprintf(r->title_id, sizeof(r->title_id), "%s", title_id);
    if (ls_title_is_running(title_id)) { r->running = 1; return 0; }
    ls_event("RECONCILE_TITLE_FULL_STARTED", title_id, "");

    ls_save_list_t l = {0};
    ls_enum_saves(&l, title_id);
    ls_save_meta_t *metas = l.count ? calloc((size_t)l.count, sizeof(*metas)) : NULL;
    if (l.count && !metas) { ls_save_list_free(&l); return -1; }
    ls_read_metas(&l, metas);
    ls_title_signature(title_id, r->signature, NULL);
    pthread_mutex_lock(&g_ls_mtx);
    r->tracked = g_ls_db ? ls_title_tracked_locked(title_id) : 0;
    pthread_mutex_unlock(&g_ls_mtx);

    for (int i = 0; i < l.count; i++) {
        if (ls_rjob_cancelled()) { r->cancelled = 1; break; }
        if (ls_title_is_running(title_id)) { r->running = 1; break; }   /* never race a running game */
        ls_rjob_progress(i, l.count, l.items[i].save_name);
        char sha[65];
        ls_full_item_t *it = ls_full_add(r);
        if (!it) break;
        snprintf(it->user_id, sizeof(it->user_id), "%s", l.items[i].user_id);
        snprintf(it->save_name, sizeof(it->save_name), "%s", l.items[i].save_name);
        if (ls_sha256_file(l.items[i].path, sha, NULL, iobuf) < 0) {
            it->status = LS_ST_MISSING_SAVE;
            snprintf(it->detail, sizeof(it->detail), "disappeared during the scan");
            continue;
        }
        char ms[256];
        ls_save_meta_str(&metas[i], ms, sizeof(ms));
        ls_full_classify(r, &l.items[i], ms, sha, it);
        /* Hash-established PS5 state vs. the PC's last report. */
        if (it->status != LS_ST_UNTRACKED) {
            ls_sync_result_t sr;
            ls_sync_classify(l.items[i].user_id, title_id, l.items[i].save_name, 1,
                             it->status == LS_ST_UNBACKED_CHANGE, it->matched_gen, &sr);
            it->sync = sr.state;
            snprintf(it->sync_detail, sizeof(it->sync_detail), "%s", sr.detail);
            if (sr.state != LS_SY_NO_PC_DATA) r->pc_compared = 1;
            r->sync_counts[sr.state]++;
        }
    }

    if (!r->cancelled && !r->running) {
        ls_unscanned_t un[32];
        int nun = ls_find_unscanned(title_id, un, 32, NULL);
        for (int i = 0; i < nun && i < 32; i++) {
            ls_full_item_t *it = ls_full_add(r);
            if (!it) break;
            snprintf(it->user_id, sizeof(it->user_id), "%s", un[i].user_id);
            snprintf(it->save_name, sizeof(it->save_name), "%s", un[i].save_name);
            it->status = LS_ST_NOT_SCANNED;
            snprintf(it->detail, sizeof(it->detail), "%s", un[i].reason);
        }

        /* Protected before, gone now. */
        pthread_mutex_lock(&g_ls_mtx);
        sqlite3_stmt *st = NULL;
        if (g_ls_db && sqlite3_prepare_v2(g_ls_db,
                "SELECT user_id, save_name FROM saves WHERE title_id = ? AND state != 'untracked'"
                " AND last_packaged_sha IS NOT NULL AND last_packaged_sha != ''", -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, title_id);
            while (sqlite3_step(st) == SQLITE_ROW) {
                char uid[16], save[256];
                ls_col_text(st, 0, uid, sizeof(uid));
                ls_col_text(st, 1, save, sizeof(save));
                int present = 0;
                for (int i = 0; i < l.count && !present; i++)
                    present = strcmp(l.items[i].user_id, uid) == 0 && strcmp(l.items[i].save_name, save) == 0;
                if (present) continue;
                ls_full_item_t *it = ls_full_add(r);
                if (!it) break;
                snprintf(it->user_id, sizeof(it->user_id), "%s", uid);
                snprintf(it->save_name, sizeof(it->save_name), "%s", save);
                it->status = LS_ST_MISSING_SAVE;
                snprintf(it->detail, sizeof(it->detail), "protected before, not on the PS5 now; archives kept");
            }
        }
        sqlite3_finalize(st);
        pthread_mutex_unlock(&g_ls_mtx);
        ls_full_check_archives(r, iobuf);
        if (ls_rjob_cancelled()) r->cancelled = 1;
    }

    r->result = LS_ST_HEALTHY;
    for (int i = 0; i < r->nitems; i++) {
        r->counts[r->items[i].status]++;
        if (r->items[i].pending_pc) r->pending_pc++;
        if (r->items[i].status > r->result) r->result = r->items[i].status;
    }
    if (r->archives_corrupt && r->result < LS_ST_QUEUE_INCONSISTENCY) r->result = LS_ST_QUEUE_INCONSISTENCY;
    ls_rjob_progress(l.count, l.count, "");
    free(metas);
    ls_save_list_free(&l);
    return 0;
}

static char *ls_full_json(const ls_full_report_t *r, int64_t at, int protect_queued) {
    size_t cap = 4096, pos = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    buf[0] = 0;
    const char *action = r->running ? "Title is running: run Full Reconcile again after closing it"
                       : r->cancelled ? "Cancelled: results are partial"
                       : ls_status_action(r->result);
    char erep[2100];
    json_escape(r->repair_log, erep, sizeof(erep));
    ls_json_append(&buf, &cap, &pos,
        "{\"title_id\":\"%s\",\"check\":\"full\",\"workload\":\"moderate\",\"checked_at\":%lld,"
        "\"running\":%s,\"cancelled\":%s,\"tracked\":%s,\"result\":\"%s\",\"recommended_action\":\"%s\","
        "\"pending_pc\":%d,\"pc_compared\":%s,\"conflicts\":%d,\"pc_newer\":%d,\"protect_job_queued\":%s,"
        "\"archives\":{\"checked\":%d,\"corrupt\":%d,\"missing\":%d},"
        "\"repairs\":%d,\"repair_log\":\"%s\",\"counts\":{",
        r->title_id, (long long)at, r->running ? "true" : "false", r->cancelled ? "true" : "false",
        r->tracked ? "true" : "false", r->running ? "UNKNOWN" : ls_status_name(r->result), action,
        r->pending_pc, r->pc_compared ? "true" : "false", r->sync_counts[LS_SY_CONFLICT],
        r->sync_counts[LS_SY_PC_NEWER], protect_queued ? "true" : "false",
        r->archives_checked, r->archives_corrupt, r->archives_missing, r->repairs, erep);
    int first = 1;
    for (int s = 0; s < LS_ST__COUNT; s++) {
        if (!r->counts[s]) continue;
        ls_json_append(&buf, &cap, &pos, "%s\"%s\":%d", first ? "" : ",", ls_status_name(s), r->counts[s]);
        first = 0;
    }
    ls_json_append(&buf, &cap, &pos, "},\"saves\":[");
    for (int i = 0; i < r->nitems; i++) {
        const ls_full_item_t *it = &r->items[i];
        char es[520], ed[500], eo[600];
        json_escape(it->save_name, es, sizeof(es));
        json_escape(it->detail, ed, sizeof(ed));
        ls_json_append(&buf, &cap, &pos,
            "%s{\"user_id\":\"%s\",%s\"save_name\":\"%s\",\"status\":\"%s\",\"sha256\":\"%s\","
            "\"current_gen\":%lld,\"matched_gen\":%lld,\"newest_gen\":%lld,\"pending_pc\":%s,\"detail\":\"%s\","
            "\"sync_state\":\"%s\",\"sync_detail\":\"%s\"}",
            i ? "," : "", it->user_id, ls_object_json(it->save_name, eo, sizeof(eo)), es, ls_status_name(it->status), it->sha,
            (long long)it->current_gen, (long long)it->matched_gen, (long long)it->newest_gen,
            it->pending_pc ? "true" : "false", ed,
            it->status == LS_ST_UNTRACKED || it->status == LS_ST_MISSING_SAVE || it->status == LS_ST_NOT_SCANNED
                ? "" : ls_sync_name(it->sync), it->sync_detail);
    }
    ls_json_append(&buf, &cap, &pos, "]}");
    return buf;
}

/* Runs Full Reconcile for one title inside the job slot. */
static void ls_full_title_run(const char *title_id, int protect, char *iobuf) {
    ls_full_report_t r;
    if (ls_full_title(title_id, &r, iobuf) < 0) {
        ls_rjob_finish("failed", "out of memory");
        return;
    }
    int queued = 0;
    if (protect && !r.running && !r.cancelled &&
        (r.counts[LS_ST_UNBACKED_CHANGE] || r.counts[LS_ST_QUEUE_INCONSISTENCY]) && g_ls_worker_started)
        queued = ls_worker_enqueue(LS_JOB_TITLE_MANUAL, title_id, 0, 0) > 0;
    char *json = ls_full_json(&r, (int64_t)time(NULL), queued);
    char summary[300];
    snprintf(summary, sizeof(summary), "%s: %s (saves=%d repairs=%d corrupt=%d%s)", title_id,
             r.running ? "UNKNOWN (title running)" : ls_status_name(r.result), r.nitems, r.repairs,
             r.archives_corrupt, queued ? ", protect job queued" : "");
    if (json && !r.cancelled && !r.running)
        ls_reconcile_store(title_id, "full", ls_status_name(r.result), ls_status_action(r.result),
                           r.signature, json);
    free(json);
    if (r.cancelled) {
        ls_event("RECONCILE_CANCELLED", title_id, "full reconcile cancelled after %d save(s)", r.nitems);
        ls_rjob_finish("cancelled", summary);
    } else {
        ls_event("RECONCILE_TITLE_FULL_COMPLETED", title_id, "%s", summary);
        if (!r.running && r.result != LS_ST_HEALTHY && r.result != LS_ST_UNTRACKED)
            ls_event("RECONCILE_ANOMALY_FOUND", title_id, "%s", ls_status_name(r.result));
        ls_rjob_finish("completed", summary);
    }
    free(r.items);
}

typedef struct { char title_id[32]; int protect; } ls_full_args_t;

static void *ls_full_thread(void *arg) {
    ls_full_args_t *a = arg;
    char *iobuf = malloc(BUF_SIZE);
    if (iobuf) ls_full_title_run(a->title_id, a->protect, iobuf);
    else ls_rjob_finish("failed", "out of memory");
    free(iobuf);
    free(a);
    return NULL;
}

/* Starts Full Reconcile in the background. 0 ok, -1 busy, -2 failure. */
static int ls_full_title_start(const char *title_id, int protect) {
    if (ls_rjob_claim("title_full", title_id, protect) < 0) return -1;
    ls_full_args_t *a = calloc(1, sizeof(*a));
    if (!a) { ls_rjob_finish("failed", "out of memory"); return -2; }
    snprintf(a->title_id, sizeof(a->title_id), "%s", title_id);
    a->protect = protect;
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    int rc = pthread_create(&tid, &attr, ls_full_thread, a);
    pthread_attr_destroy(&attr);
    if (rc != 0) { free(a); ls_rjob_finish("failed", "cannot start thread"); return -2; }
    pthread_detach(tid);
    return 0;
}

/* ── Catalogue jobs: Quick Catalogue Check (I) / Full Catalogue (J) ── */
/*
 * Run in the single job slot, one title after another, cancellable between
 * titles. Titles come from three sources so nothing falls through: saves the
 * scanner sees, titles Shallot protected before (catches titles whose saves
 * vanished entirely) and titles in the PS5 save database the scanner cannot
 * see. A title that is running when its turn comes is skipped and listed.
 * Quick = Sanity Check per title (metadata only). Full = Full Reconcile per
 * title; with protect=1 a normal protect job is queued only for titles that
 * have unprotected content.
 */
typedef struct { char id[32]; } ls_tid_t;

static int ls_tid_cmp(const void *a, const void *b) { return strcmp(((const ls_tid_t *)a)->id, ((const ls_tid_t *)b)->id); }

static int ls_tid_add(ls_tid_t **v, int *n, int *cap, const char *id) {
    for (int i = 0; i < *n; i++) if (strcmp((*v)[i].id, id) == 0) return 0;
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 64;
        ls_tid_t *p = realloc(*v, sizeof(*p) * (size_t)nc);
        if (!p) return -1;
        *v = p;
        *cap = nc;
    }
    snprintf((*v)[(*n)++].id, sizeof((*v)[0].id), "%s", id);
    return 1;
}

static int ls_catalogue_titles(ls_tid_t **out) {
    ls_tid_t *v = NULL;
    int n = 0, cap = 0;
    ls_save_list_t l = {0};
    ls_enum_saves(&l, NULL);
    for (int i = 0; i < l.count; i++) ls_tid_add(&v, &n, &cap, l.items[i].title_id);
    ls_save_list_free(&l);
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *st = NULL;
    if (g_ls_db && sqlite3_prepare_v2(g_ls_db,
            "SELECT DISTINCT title_id FROM saves WHERE last_packaged_sha IS NOT NULL AND last_packaged_sha != ''",
            -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *t = sqlite3_column_text(st, 0);
            if (t) ls_tid_add(&v, &n, &cap, (const char *)t);
        }
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    ls_unscanned_t *un = calloc(256, sizeof(*un));
    if (un) {
        int k = ls_find_unscanned(NULL, un, 256, NULL);
        for (int i = 0; i < k && i < 256; i++) ls_tid_add(&v, &n, &cap, un[i].title_id);
        free(un);
    }
    if (n > 1) qsort(v, (size_t)n, sizeof(*v), ls_tid_cmp);
    *out = v;
    return n;
}

static void ls_meta_set_text(const char *key, const char *value) {
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)",
                               -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, key);
            ls_bind_text(st, 2, value);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
}

static char *ls_meta_get_text(const char *key) {
    char *v = NULL;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "SELECT value FROM meta WHERE key = ?", -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, key);
            if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
                v = strdup((const char *)sqlite3_column_text(st, 0));
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    return v;
}

typedef struct {
    int full, protect;
    int titles, checked, skipped_running, cancelled, protect_jobs;
    int by_result[LS_ST__COUNT];
    int changed_saves, regressions, unprotected, missing, not_scanned, conflicts, errors;
    size_t cap, pos;
    char *anom;          /* JSON array body of anomalous titles */
} ls_cat_t;

static void ls_cat_note(ls_cat_t *c, const char *tid, ls_save_status_t result, const char *extra) {
    c->by_result[result]++;
    if (result == LS_ST_HEALTHY || result == LS_ST_UNTRACKED) return;
    const char *name = lookup_title(tid);
    char en[520];
    json_escape(name ? name : "", en, sizeof(en));
    ls_json_append(&c->anom, &c->cap, &c->pos, "%s{\"title_id\":\"%s\",\"name\":\"%s\",\"result\":\"%s\",\"detail\":\"%s\"}",
                   c->pos ? "," : "", tid, en, ls_status_name(result), extra ? extra : "");
    ls_event("RECONCILE_ANOMALY_FOUND", tid, "%s (%s)", ls_status_name(result), c->full ? "catalogue full" : "catalogue quick");
}

static void ls_cat_counts(ls_cat_t *c, const int *counts) {
    c->changed_saves += counts[LS_ST_CHANGED_SINCE_LAST_SCAN] + counts[LS_ST_UNBACKED_CHANGE];
    c->unprotected += counts[LS_ST_UNBACKED_CHANGE];
    c->regressions += counts[LS_ST_KNOWN_REGRESSION];
    c->missing += counts[LS_ST_MISSING_SAVE];
    c->not_scanned += counts[LS_ST_NOT_SCANNED];
    c->errors += counts[LS_ST_QUEUE_INCONSISTENCY] + counts[LS_ST_MANIFEST_INCONSISTENCY];
}

static char *ls_cat_summary_json(const ls_cat_t *c) {
    size_t cap = 2048, pos = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    buf[0] = 0;
    ls_json_append(&buf, &cap, &pos,
        "{\"kind\":\"%s\",\"workload\":\"%s\",\"protect\":%s,\"finished_at\":%lld,\"cancelled\":%s,"
        "\"titles\":%d,\"checked\":%d,\"skipped_running\":%d,"
        "\"changed_saves\":%d,\"unprotected_changes\":%d,\"known_regressions\":%d,\"missing_saves\":%d,"
        "\"not_scanned\":%d,\"conflicts\":%d,\"errors\":%d,\"protect_jobs_queued\":%d,\"by_result\":{",
        c->full ? "catalogue_full" : "catalogue_quick", c->full ? "deep" : "quick",
        c->protect ? "true" : "false", (long long)time(NULL), c->cancelled ? "true" : "false",
        c->titles, c->checked, c->skipped_running, c->changed_saves, c->unprotected, c->regressions,
        c->missing, c->not_scanned, c->conflicts, c->errors, c->protect_jobs);
    int first = 1;
    for (int s = 0; s < LS_ST__COUNT; s++) {
        if (!c->by_result[s]) continue;
        ls_json_append(&buf, &cap, &pos, "%s\"%s\":%d", first ? "" : ",", ls_status_name(s), c->by_result[s]);
        first = 0;
    }
    ls_json_append(&buf, &cap, &pos, "},\"anomalous_titles\":[%s]}", c->anom ? c->anom : "");
    return buf;
}

static void ls_catalogue_run(int full, int protect, char *iobuf) {
    ls_cat_t c;
    memset(&c, 0, sizeof(c));
    c.full = full;
    c.protect = protect;
    c.cap = 1024;
    c.anom = malloc(c.cap);
    if (c.anom) c.anom[0] = 0;
    ls_tid_t *titles = NULL;
    c.titles = ls_catalogue_titles(&titles);
    ls_event(full ? "RECONCILE_CATALOGUE_FULL_STARTED" : "RECONCILE_CATALOGUE_QUICK_STARTED", NULL,
             "%d titles%s", c.titles, protect ? ", protect changed saves" : "");

    for (int i = 0; i < c.titles; i++) {
        if (ls_rjob_cancelled()) { c.cancelled = 1; break; }
        const char *tid = titles[i].id;
        ls_rjob_progress(i, c.titles, tid);
        if (ls_title_is_running(tid)) { c.skipped_running++; continue; }
        if (!full) {
            ls_sanity_report_t r;
            if (ls_sanity_title(tid, &r) < 0) { c.errors++; continue; }
            char *json = ls_sanity_json(&r, (int64_t)time(NULL));
            if (json) ls_reconcile_store(tid, "catalogue", ls_status_name(r.result), ls_status_action(r.result),
                                         r.signature, json);
            free(json);
            ls_cat_counts(&c, r.counts);
            ls_cat_note(&c, tid, r.result, r.pending_pc ? "PC download pending" : "");
            ls_sanity_free(&r);
        } else {
            ls_full_report_t r;
            if (ls_full_title(tid, &r, iobuf) < 0) { c.errors++; continue; }
            if (r.running) { c.skipped_running++; free(r.items); continue; }   /* started meanwhile */
            if (r.cancelled) { c.cancelled = 1; free(r.items); break; }
            int queued = 0;
            if (protect && (r.counts[LS_ST_UNBACKED_CHANGE] || r.counts[LS_ST_QUEUE_INCONSISTENCY]) &&
                g_ls_worker_started && ls_worker_enqueue(LS_JOB_TITLE_MANUAL, tid, 0, 0) > 0) {
                queued = 1;
                c.protect_jobs++;
            }
            char *json = ls_full_json(&r, (int64_t)time(NULL), queued);
            if (json) ls_reconcile_store(tid, "catalogue", ls_status_name(r.result), ls_status_action(r.result),
                                         r.signature, json);
            free(json);
            ls_cat_counts(&c, r.counts);
            c.errors += r.archives_corrupt;
            c.conflicts += r.sync_counts[LS_SY_CONFLICT];
            if (r.sync_counts[LS_SY_CONFLICT])
                ls_event("SYNC_CONFLICT_DETECTED", tid, "%d save(s) (catalogue full)", r.sync_counts[LS_SY_CONFLICT]);
            ls_cat_note(&c, tid, r.result, queued ? "protect job queued" : "");
            free(r.items);
        }
        c.checked++;
        if (full && c.checked % 10 == 0)
            ls_event("RECONCILE_CATALOGUE_PROGRESS", NULL, "%d / %d titles, changed=%d regressions=%d unprotected=%d",
                     c.checked, c.titles, c.changed_saves, c.regressions, c.unprotected);
    }
    ls_rjob_progress(c.checked, c.titles, "");

    char *json = ls_cat_summary_json(&c);
    char summary[300];
    snprintf(summary, sizeof(summary),
             "%d/%d titles: %d healthy, %d changed, %d missing, %d regressions, %d not scanned, %d errors%s%s",
             c.checked, c.titles, c.by_result[LS_ST_HEALTHY], c.changed_saves, c.missing, c.regressions,
             c.not_scanned, c.errors, c.skipped_running ? ", running skipped" : "",
             c.protect_jobs ? ", protect jobs queued" : "");
    if (json) ls_meta_set_text(full ? "last_catalogue_full" : "last_catalogue_quick", json);
    ls_meta_set_int(full ? "last_catalogue_full_at" : "last_catalogue_quick_at", (int64_t)time(NULL));
    free(json);
    if (c.cancelled) {
        ls_event("RECONCILE_CANCELLED", NULL, "catalogue %s after %d title(s)", full ? "full" : "quick", c.checked);
        ls_rjob_finish("cancelled", summary);
    } else {
        ls_event(full ? "RECONCILE_CATALOGUE_FULL_COMPLETED" : "RECONCILE_CATALOGUE_QUICK_COMPLETED", NULL, "%s", summary);
        ls_rjob_finish("completed", summary);
    }
    free(titles);
    free(c.anom);
}

typedef struct { int full, protect; } ls_cat_args_t;

static void *ls_catalogue_thread(void *arg) {
    ls_cat_args_t *a = arg;
    char *iobuf = malloc(BUF_SIZE);
    if (iobuf) ls_catalogue_run(a->full, a->protect, iobuf);
    else ls_rjob_finish("failed", "out of memory");
    free(iobuf);
    free(a);
    return NULL;
}

/* 0 started, -1 another job running, -2 failure. */
static int ls_catalogue_start(int full, int protect) {
    if (ls_rjob_claim(full ? "catalogue_full" : "catalogue_quick", "", protect) < 0) return -1;
    ls_cat_args_t *a = calloc(1, sizeof(*a));
    if (!a) { ls_rjob_finish("failed", "out of memory"); return -2; }
    a->full = full;
    a->protect = protect;
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    int rc = pthread_create(&tid, &attr, ls_catalogue_thread, a);
    pthread_attr_destroy(&attr);
    if (rc != 0) { free(a); ls_rjob_finish("failed", "cannot start thread"); return -2; }
    pthread_detach(tid);
    return 0;
}
