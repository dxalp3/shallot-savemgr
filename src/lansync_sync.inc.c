/*
 * LAN save sync — PC manifest exchange and sync classification (stage L).
 * Included from main.c after lansync_worker.inc.c, before the reconcile code.
 *
 * The PC client reports, per title, every archive it holds: the archive's
 * SHA-256, the SHA-256 of the save container inside it, its queue id (if it
 * came from this PS5) and when it was created. Shallot resolves each entry to
 * one of its generations (by queue id, else by container hash) or marks it
 * "foreign" (a version this PS5 never produced, e.g. dropped into the PC
 * backup folder). Nothing here reads or writes saves.
 *
 * Classification per save (decided by ancestry, never by timestamps alone):
 *   G  = newest PS5 generation of this save that the PC holds (last common)
 *   F  = foreign PC versions created after G (PC-side changes)
 *   PS5 changed = current content is newer than G or not protected at all
 *     SYNCED         PS5 at G, no F
 *     PS5_NEWER      PS5 changed, no F
 *     PC_NEWER       PS5 at G, F exists
 *     CONFLICT       PS5 changed and F exists, or no common version at all
 *     KNOWN_REGRESSION  PS5 current is an older generation than G
 *     MISSING_ON_PC  the PC holds no version of this save
 *     MISSING_ON_PS5 the PC holds versions, the save is gone from the PS5
 *     NO_PC_DATA     the PC has not reported this title yet
 */

typedef enum {
    LS_SY_NO_PC_DATA = 0, LS_SY_SYNCED, LS_SY_PS5_NEWER, LS_SY_MISSING_ON_PC,
    LS_SY_PC_NEWER, LS_SY_MISSING_ON_PS5, LS_SY_KNOWN_REGRESSION, LS_SY_CONFLICT, LS_SY__COUNT
} ls_sync_state_t;

static const char *ls_sync_name(ls_sync_state_t s) {
    static const char *n[LS_SY__COUNT] = {
        "NO_PC_DATA", "SYNCED", "PS5_NEWER", "MISSING_ON_PC", "PC_NEWER", "MISSING_ON_PS5",
        "KNOWN_REGRESSION", "CONFLICT"
    };
    return (s >= 0 && s < LS_SY__COUNT) ? n[s] : "UNKNOWN";
}

static const char *ls_sync_action(ls_sync_state_t s) {
    switch (s) {
    case LS_SY_SYNCED: return "No action needed";
    case LS_SY_PS5_NEWER: return "PS5 is newer: the PC will pick it up (or Back Up Changed Saves if unprotected)";
    case LS_SY_MISSING_ON_PC: return "Not on the PC yet: run the PC client";
    case LS_SY_PC_NEWER: return "PC holds a newer version and the PS5 has not changed since: restore from PC is possible";
    case LS_SY_MISSING_ON_PS5: return "Save is gone from the PS5; the PC holds versions of it";
    case LS_SY_KNOWN_REGRESSION: return "Recovery available: the PC holds a newer generation than the PS5 has now";
    case LS_SY_CONFLICT: return "Conflict: PS5 and PC changed independently - choose Keep PS5 / Restore PC / Preserve both";
    default: return "Run the PC client so it reports what it holds";
    }
}

/* ── Receiving the PC manifest ──────────────────────────────────── */
/* Minimal scanner over a flat JSON array of flat objects:
 * {"archives":[{"k":"v","n":123,...},...]}. Values must not contain '}'. */
static const char *ls_next_object(const char *p, char *obj, size_t obj_sz) {
    const char *a = strchr(p, '{');
    if (!a) return NULL;
    const char *b = strchr(a + 1, '}');
    if (!b) return NULL;
    size_t len = (size_t)(b - a + 1);
    if (len >= obj_sz) len = obj_sz - 1;
    memcpy(obj, a, len);
    obj[len] = 0;
    return b + 1;
}

static int ls_hex64(const char *s) {
    if (strlen(s) != 64) return 0;
    for (const char *p = s; *p; p++) if (!isxdigit((unsigned char)*p)) return 0;
    return 1;
}

/*
 * Replaces the PC manifest rows of one title. Returns number of entries
 * stored, -1 on error. Also records PC acknowledgement for PS5 generations
 * the PC holds but Shallot had not seen acknowledged (e.g. after a DB loss).
 */
static int ls_pc_manifest_store(const char *title_id, const char *json, int *foreign_out, int *repaired_out) {
    const char *arr = strstr(json, "\"archives\"");
    if (!arr) return -1;
    arr = strchr(arr, '[');
    if (!arr) return -1;
    int stored = 0, foreign = 0, repaired = 0;
    int64_t now = (int64_t)time(NULL);
    pthread_mutex_lock(&g_ls_mtx);
    if (!g_ls_db || g_ls_schema < 5) { pthread_mutex_unlock(&g_ls_mtx); return -1; }
    ls_exec_locked("BEGIN");
    sqlite3_stmt *del = NULL;
    if (sqlite3_prepare_v2(g_ls_db, "DELETE FROM pc_manifest WHERE title_id = ?", -1, &del, NULL) == SQLITE_OK) {
        ls_bind_text(del, 1, title_id);
        sqlite3_step(del);
    }
    sqlite3_finalize(del);
    char obj[2048];
    const char *p = arr;
    while ((p = ls_next_object(p, obj, sizeof(obj))) != NULL) {
        char qid[200] = {0}, ash[80] = {0}, csh[80] = {0}, save[256] = {0}, uid[32] = {0}, tid[32] = {0}, origin[16] = {0};
        json_get_string(obj, "queue_id", qid, sizeof(qid));
        json_get_string(obj, "archive_sha256", ash, sizeof(ash));
        json_get_string(obj, "container_sha256", csh, sizeof(csh));
        json_get_string(obj, "save_name", save, sizeof(save));
        json_get_string(obj, "user_id", uid, sizeof(uid));
        json_get_string(obj, "title_id", tid, sizeof(tid));
        json_get_string(obj, "origin", origin, sizeof(origin));
        int64_t created = json_get_int_default(obj, "created_at", 0);
        if (!ls_hex64(ash) || !save[0] || (tid[0] && strcmp(tid, title_id) != 0)) continue;
        for (char *c = ash; *c; c++) *c = (char)tolower((unsigned char)*c);
        for (char *c = csh; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (csh[0] && !ls_hex64(csh)) csh[0] = 0;

        /* Resolve to a PS5 generation: queue id first, then container hash. */
        int64_t gen = 0;
        if (qid[0]) gen = ls_db_int_locked("SELECT COALESCE(gen_id,0) FROM queue WHERE id = ? AND sha256 = ?",
                                           qid, ash, NULL, 0);
        if (!gen && csh[0])
            gen = ls_db_int_locked("SELECT COALESCE(MAX(gen_id),0) FROM generations WHERE title_id = ? AND save_name = ?"
                                   " AND sha256 = ?", title_id, save, csh, 0);
        if (!gen) foreign++;

        sqlite3_stmt *ins = NULL;
        if (sqlite3_prepare_v2(g_ls_db,
                "INSERT OR REPLACE INTO pc_manifest(title_id, archive_sha256, save_name, user_id, container_sha256,"
                " queue_id, created_at, origin, gen_id, received_at) VALUES(?,?,?,?,?,?,?,?,?,?)",
                -1, &ins, NULL) == SQLITE_OK) {
            ls_bind_text(ins, 1, title_id);
            ls_bind_text(ins, 2, ash);
            ls_bind_text(ins, 3, save);
            ls_bind_text(ins, 4, uid);
            ls_bind_text(ins, 5, csh);
            ls_bind_text(ins, 6, qid);
            sqlite3_bind_int64(ins, 7, (sqlite3_int64)created);
            ls_bind_text(ins, 8, origin[0] ? origin : (gen ? "ps5" : "external"));
            sqlite3_bind_int64(ins, 9, (sqlite3_int64)gen);
            sqlite3_bind_int64(ins, 10, (sqlite3_int64)now);
            if (sqlite3_step(ins) == SQLITE_DONE) stored++;
        }
        sqlite3_finalize(ins);
        if (gen) {
            sqlite3_stmt *up = NULL;
            if (sqlite3_prepare_v2(g_ls_db, "UPDATE generations SET pc_acked_at = ? WHERE gen_id = ? AND pc_acked_at = 0",
                                   -1, &up, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(up, 1, (sqlite3_int64)now);
                sqlite3_bind_int64(up, 2, (sqlite3_int64)gen);
                sqlite3_step(up);
                repaired += sqlite3_changes(g_ls_db);
            }
            sqlite3_finalize(up);
        }
    }
    sqlite3_stmt *pt = NULL;
    if (sqlite3_prepare_v2(g_ls_db, "INSERT OR REPLACE INTO pc_titles(title_id, received_at, archives) VALUES(?,?,?)",
                           -1, &pt, NULL) == SQLITE_OK) {
        ls_bind_text(pt, 1, title_id);
        sqlite3_bind_int64(pt, 2, (sqlite3_int64)now);
        sqlite3_bind_int(pt, 3, stored);
        sqlite3_step(pt);
    }
    sqlite3_finalize(pt);
    ls_exec_locked("COMMIT");
    pthread_mutex_unlock(&g_ls_mtx);
    if (foreign_out) *foreign_out = foreign;
    if (repaired_out) *repaired_out = repaired;
    return stored;
}

/* ── Classification ─────────────────────────────────────────────── */
typedef struct {
    ls_sync_state_t state;
    int64_t last_common;      /* newest PS5 generation the PC holds */
    int64_t current_gen;
    int pc_versions, foreign_newer;
    char detail[220];
} ls_sync_result_t;

static int ls_pc_has_title_locked(const char *title_id) {
    return ls_db_int_locked("SELECT COUNT(*) FROM pc_titles WHERE title_id = ?", title_id, NULL, NULL, 0) > 0;
}

/*
 * exists: the save is on the PS5 now. ps5_unprotected: its current content
 * matches no archived generation (changed and not backed up). current_gen:
 * the generation the PS5 content corresponds to (0 if unknown).
 */
static void ls_sync_classify(const char *user_id, const char *title_id, const char *save_name, int exists,
                             int ps5_unprotected, int64_t current_gen, ls_sync_result_t *r) {
    memset(r, 0, sizeof(*r));
    r->current_gen = current_gen;
    pthread_mutex_lock(&g_ls_mtx);
    if (!g_ls_db || g_ls_schema < 5 || !ls_pc_has_title_locked(title_id)) {
        pthread_mutex_unlock(&g_ls_mtx);
        r->state = LS_SY_NO_PC_DATA;
        snprintf(r->detail, sizeof(r->detail), "the PC has not reported this title yet");
        return;
    }
    /* user_id '' in the manifest (external archives) matches any user. */
    const char *uid = user_id ? user_id : "";
    r->pc_versions = (int)ls_db_int_locked(
        "SELECT COUNT(*) FROM pc_manifest WHERE title_id = ? AND save_name = ? AND (user_id = ? OR user_id = '')",
        title_id, save_name, uid, 0);
    r->last_common = ls_db_int_locked(
        "SELECT COALESCE(MAX(gen_id),0) FROM pc_manifest WHERE title_id = ? AND save_name = ? AND (user_id = ? OR user_id = '')",
        title_id, save_name, uid, 0);
    int64_t common_created = 0;
    if (r->last_common) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "SELECT COALESCE(created_at,0) FROM generations WHERE gen_id = ?", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)r->last_common);
            if (sqlite3_step(st) == SQLITE_ROW) common_created = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
    }
    r->foreign_newer = (int)ls_db_int_locked(
        "SELECT COUNT(*) FROM pc_manifest WHERE title_id = ? AND save_name = ? AND (user_id = ? OR user_id = '')"
        " AND gen_id = 0 AND created_at > ?", title_id, save_name, uid, common_created);
    pthread_mutex_unlock(&g_ls_mtx);

    if (!exists) {
        r->state = r->pc_versions ? LS_SY_MISSING_ON_PS5 : LS_SY_NO_PC_DATA;
        snprintf(r->detail, sizeof(r->detail), "%d version(s) on the PC", r->pc_versions);
        return;
    }
    if (!r->pc_versions) {
        r->state = LS_SY_MISSING_ON_PC;
        snprintf(r->detail, sizeof(r->detail), "the PC holds no version of this save");
        return;
    }
    if (!r->last_common) {
        /* PC only holds versions this PS5 never produced: no shared ancestor. */
        r->state = LS_SY_CONFLICT;
        snprintf(r->detail, sizeof(r->detail), "PC holds %d version(s) with no common ancestor", r->pc_versions);
        return;
    }
    if (!ps5_unprotected && current_gen && current_gen < r->last_common) {
        r->state = LS_SY_KNOWN_REGRESSION;
        snprintf(r->detail, sizeof(r->detail), "PS5 = gen %lld, PC holds newer gen %lld",
                 (long long)current_gen, (long long)r->last_common);
        return;
    }
    int ps5_changed = ps5_unprotected || current_gen > r->last_common;
    if (!ps5_changed && !r->foreign_newer) {
        r->state = LS_SY_SYNCED;
        snprintf(r->detail, sizeof(r->detail), "both at gen %lld", (long long)r->last_common);
    } else if (ps5_changed && !r->foreign_newer) {
        r->state = LS_SY_PS5_NEWER;
        snprintf(r->detail, sizeof(r->detail), "%s; last common gen %lld",
                 ps5_unprotected ? "PS5 has an unprotected change" : "PC download pending", (long long)r->last_common);
    } else if (!ps5_changed) {
        r->state = LS_SY_PC_NEWER;
        snprintf(r->detail, sizeof(r->detail), "PC has %d newer version(s) after gen %lld",
                 r->foreign_newer, (long long)r->last_common);
    } else {
        r->state = LS_SY_CONFLICT;
        snprintf(r->detail, sizeof(r->detail), "both changed since gen %lld", (long long)r->last_common);
    }
}

/* ── Compare With PC (cheap, metadata + Shallot's records) ──────── */
static char *ls_compare_title_json(const char *title_id) {
    ls_save_list_t l = {0};
    ls_enum_saves(&l, title_id);
    ls_save_meta_t *metas = l.count ? calloc((size_t)l.count, sizeof(*metas)) : NULL;
    if (l.count && !metas) { ls_save_list_free(&l); return NULL; }
    ls_read_metas(&l, metas);
    size_t cap = 4096, pos = 0;
    char *buf = malloc(cap);
    if (!buf) { free(metas); ls_save_list_free(&l); return NULL; }
    buf[0] = 0;
    int counts[LS_SY__COUNT] = {0};
    int64_t reported = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db && g_ls_schema >= 5)
        reported = ls_db_int_locked("SELECT COALESCE(MAX(received_at),0) FROM pc_titles WHERE title_id = ?",
                                    title_id, NULL, NULL, 0);
    pthread_mutex_unlock(&g_ls_mtx);
    ls_json_append(&buf, &cap, &pos, "{\"title_id\":\"%s\",\"check\":\"compare_pc\",\"workload\":\"quick\","
                   "\"pc_reported_at\":%lld,\"saves\":[", title_id, (long long)reported);
    int first = 1;
    for (int i = 0; i < l.count; i++) {
        char ms[256];
        ls_save_meta_str(&metas[i], ms, sizeof(ms));
        ls_save_row_t row;
        ls_saves_get(&l.items[i], &row);
        int unprot = !row.found || !row.last_packaged_meta[0] || strcmp(row.last_packaged_meta, ms) != 0 ||
                     strcmp(row.state, "protected") != 0;
        ls_sync_result_t r;
        ls_sync_classify(l.items[i].user_id, title_id, l.items[i].save_name, 1, unprot, row.current_gen_id, &r);
        counts[r.state]++;
        char es[520], ed[460], eo[600];
        json_escape(l.items[i].save_name, es, sizeof(es));
        json_escape(r.detail, ed, sizeof(ed));
        ls_json_append(&buf, &cap, &pos,
            "%s{\"user_id\":\"%s\",%s\"save_name\":\"%s\",\"sync_state\":\"%s\",\"current_gen\":%lld,"
            "\"last_common_gen\":%lld,\"pc_versions\":%d,\"metadata_changed\":%s,\"detail\":\"%s\",\"action\":\"%s\"}",
            first ? "" : ",", l.items[i].user_id, ls_object_json(l.items[i].save_name, eo, sizeof(eo)), es,
            ls_sync_name(r.state), (long long)r.current_gen,
            (long long)r.last_common, r.pc_versions, unprot ? "true" : "false", ed, ls_sync_action(r.state));
        first = 0;
    }
    /* Saves only the PC still has. */
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *st = NULL;
    if (g_ls_db && g_ls_schema >= 5 && sqlite3_prepare_v2(g_ls_db,
            "SELECT save_name, MAX(user_id), COUNT(*) FROM pc_manifest WHERE title_id = ? GROUP BY save_name",
            -1, &st, NULL) == SQLITE_OK) {
        ls_bind_text(st, 1, title_id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            char save[256];
            ls_col_text(st, 0, save, sizeof(save));
            int present = 0;
            for (int i = 0; i < l.count && !present; i++) present = strcmp(l.items[i].save_name, save) == 0;
            if (present) continue;
            char es[520], uid[32], eo[600];
            ls_col_text(st, 1, uid, sizeof(uid));
            json_escape(save, es, sizeof(es));
            counts[LS_SY_MISSING_ON_PS5]++;
            ls_json_append(&buf, &cap, &pos,
                "%s{\"user_id\":\"%s\",%s\"save_name\":\"%s\",\"sync_state\":\"MISSING_ON_PS5\",\"current_gen\":0,"
                "\"last_common_gen\":0,\"pc_versions\":%d,\"metadata_changed\":false,"
                "\"detail\":\"only on the PC\",\"action\":\"%s\"}",
                first ? "" : ",", uid, ls_object_json(save, eo, sizeof(eo)), es, sqlite3_column_int(st, 2),
                ls_sync_action(LS_SY_MISSING_ON_PS5));
            first = 0;
        }
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);

    ls_sync_state_t worst = LS_SY_SYNCED;
    int any = 0;
    for (int s = 0; s < LS_SY__COUNT; s++) if (counts[s]) { any = 1; if (s > (int)worst) worst = (ls_sync_state_t)s; }
    if (!any || (counts[LS_SY_NO_PC_DATA] && worst == LS_SY_NO_PC_DATA)) worst = LS_SY_NO_PC_DATA;
    ls_json_append(&buf, &cap, &pos, "],\"result\":\"%s\",\"recommended_action\":\"%s\",\"counts\":{",
                   ls_sync_name(worst), ls_sync_action(worst));
    first = 1;
    for (int s = 0; s < LS_SY__COUNT; s++) {
        if (!counts[s]) continue;
        ls_json_append(&buf, &cap, &pos, "%s\"%s\":%d", first ? "" : ",", ls_sync_name(s), counts[s]);
        first = 0;
    }
    ls_json_append(&buf, &cap, &pos, "}}");
    if (counts[LS_SY_CONFLICT]) ls_event("SYNC_CONFLICT_DETECTED", title_id, "%d save(s)", counts[LS_SY_CONFLICT]);
    free(metas);
    ls_save_list_free(&l);
    return buf;
}
