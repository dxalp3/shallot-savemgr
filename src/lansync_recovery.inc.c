/*
 * LAN save sync — regression recovery (stage N).
 * Included from main.c after lansync_restore.inc.c.
 *
 * A protect run that finds a save whose current content equals an OLDER
 * archived generation than the newest one (e.g. a kernel panic left the
 * previous save on disk) hands the regressed files here. Per save object a
 * recovery offer is recorded: current state "A" (already archived, nothing
 * is lost) and target "B" = the newest snapshot before this run that held
 * the newer generations.
 *
 * An offer is "certain" only if every regressed file's current generation
 * is an ancestor of its target generation, no file of the object got new
 * (unknown) content in the same run, and every target archive is still on
 * the PS5. Policy (sync.json regression_recovery):
 *   off     record + event only
 *   ask     record + notification; the owner chooses Restore B / Keep A
 *   certain restore B automatically when the offer is certain, else ask
 * Deliberate restores / deletes never create offers (their protect runs use
 * the reasons pre_restore / restored / pre_delete / deleted).
 */

static int ls_recovery_skip_reason(const char *reason) {
    return strcmp(reason, "pre_restore") == 0 || strcmp(reason, "restored") == 0 ||
           strcmp(reason, "pre_delete") == 0 || strcmp(reason, "deleted") == 0;
}

/* 1 if `anc` is reached by following parent_gen from `gen` (bounded). */
static int ls_gen_is_ancestor_locked(int64_t anc, int64_t gen) {
    for (int i = 0; i < 256 && gen > 0; i++) {
        gen = ls_db_int_locked("SELECT COALESCE(parent_gen, 0) FROM generations WHERE ? = ? AND ? IS NOT NULL AND gen_id = ?",
                               "x", "x", "x", gen);
        if (gen == anc) return 1;
    }
    return 0;
}

/* Generation a snapshot recorded for one file (0 if not in it). */
static int64_t ls_snap_gen_locked(int64_t snap, const char *save_name) {
    return ls_db_int_locked("SELECT COALESCE(MAX(gen_id), 0) FROM snapshot_files WHERE save_name = ? AND ? IS NOT NULL"
                            " AND ? IS NOT NULL AND snap_id = ?", save_name, "x", "x", snap);
}

static void ls_recovery_process(const char *title_id, const char *reason, const ls_regress_list_t *col, int64_t prev_snap) {
    if (!col || !col->n || ls_recovery_skip_reason(reason) || g_ls_schema < 7) return;
    ls_config_t cfg;
    ls_load_config(&cfg);
    int64_t now = (int64_t)time(NULL);

    /* New, unknown content on an object with an open offer: restoring B would
     * now also replace that progress (it is archived) -> flag the offer. */
    for (int i = 0; i < col->n; i++) {
        if (!col->items[i].changed) continue;
        pthread_mutex_lock(&g_ls_mtx);
        sqlite3_stmt *st = NULL;
        if (g_ls_db && sqlite3_prepare_v2(g_ls_db, "UPDATE recovery SET stale = 1 WHERE title_id = ? AND user_id = ?"
                                          " AND object_id = ? AND state = 'open'", -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, title_id);
            ls_bind_text(st, 2, col->items[i].user_id);
            ls_bind_text(st, 3, col->items[i].object_id);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
        pthread_mutex_unlock(&g_ls_mtx);
    }

    for (int i = 0; i < col->n; i++) {
        const ls_regress_t *ri = &col->items[i];
        if (ri->changed) continue;
        int first = 1;
        for (int k = 0; k < i; k++)
            if (!col->items[k].changed && strcmp(col->items[k].user_id, ri->user_id) == 0 &&
                strcmp(col->items[k].object_id, ri->object_id) == 0) first = 0;
        if (!first) continue;

        /* Target: newest snapshot before this run holding this file's newest generation,
         * otherwise any newer generation of it. */
        int64_t target = 0;
        pthread_mutex_lock(&g_ls_mtx);
        if (g_ls_db) {
            sqlite3_stmt *st = NULL;
            const char *q[2] = {
                "SELECT COALESCE(MAX(s.snap_id), 0) FROM snapshots s JOIN snapshot_files f ON f.snap_id = s.snap_id"
                " WHERE s.title_id = ? AND s.user_id = ? AND f.save_name = ? AND s.snap_id <= ? AND f.gen_id = ?",
                "SELECT COALESCE(MAX(s.snap_id), 0) FROM snapshots s JOIN snapshot_files f ON f.snap_id = s.snap_id"
                " WHERE s.title_id = ? AND s.user_id = ? AND f.save_name = ? AND s.snap_id <= ? AND f.gen_id > ?" };
            int64_t want[2] = { ri->newest_gen, ri->cur_gen };
            for (int v = 0; v < 2 && !target; v++) {
                st = NULL;
                if (sqlite3_prepare_v2(g_ls_db, q[v], -1, &st, NULL) == SQLITE_OK) {
                    ls_bind_text(st, 1, title_id);
                    ls_bind_text(st, 2, ri->user_id);
                    ls_bind_text(st, 3, ri->save_name);
                    sqlite3_bind_int64(st, 4, (sqlite3_int64)prev_snap);
                    sqlite3_bind_int64(st, 5, (sqlite3_int64)want[v]);
                    if (sqlite3_step(st) == SQLITE_ROW) target = sqlite3_column_int64(st, 0);
                }
                sqlite3_finalize(st);
            }
        }
        /* Certainty + description over every regressed file of the object. */
        int certain = target > 0, on_ps5 = target > 0, pc = target > 0;
        char files[600] = {0}, why[200] = {0};
        size_t fp = 0;
        for (int k = 0; k < col->n; k++) {
            const ls_regress_t *rk = &col->items[k];
            if (strcmp(rk->user_id, ri->user_id) != 0 || strcmp(rk->object_id, ri->object_id) != 0) continue;
            if (rk->changed) { certain = 0; if (!why[0]) snprintf(why, sizeof(why), "%s also has new content", rk->save_name); continue; }
            int64_t tg = target ? ls_snap_gen_locked(target, rk->save_name) : 0;
            if (fp < sizeof(files) - 1)
                fp += (size_t)snprintf(files + fp, sizeof(files) - fp, "%s%s gen %lld -> %lld", fp ? "; " : "", rk->save_name,
                                       (long long)rk->cur_gen, (long long)tg);
            if (fp >= sizeof(files)) fp = sizeof(files) - 1;
            if (!tg || tg <= rk->cur_gen) { certain = 0; if (!why[0]) snprintf(why, sizeof(why), "no newer version of %s in a snapshot", rk->save_name); }
            else if (!ls_gen_is_ancestor_locked(rk->cur_gen, tg)) {
                certain = 0;
                if (!why[0]) snprintf(why, sizeof(why), "%s: current version is not an ancestor of the newer one", rk->save_name);
            }
        }
        if (target) {
            int64_t missing = ls_db_int_locked("SELECT COUNT(*) FROM snapshot_files f JOIN generations g ON g.gen_id = f.gen_id"
                                               " LEFT JOIN queue q ON q.id = g.queue_id WHERE f.object_id = ? AND ? IS NOT NULL"
                                               " AND ? IS NOT NULL AND COALESCE(q.state, '') NOT IN ('ready','packed') AND f.snap_id = ?",
                                               ri->object_id, "x", "x", target);
            if (missing > 0) { on_ps5 = 0; certain = 0; if (!why[0]) snprintf(why, sizeof(why), "the newer version is only on the PC"); }
            pc = ls_db_int_locked("SELECT COUNT(*) FROM snapshot_files f JOIN generations g ON g.gen_id = f.gen_id"
                                  " WHERE f.object_id = ? AND ? IS NOT NULL AND ? IS NOT NULL AND g.pc_acked_at = 0"
                                  " AND f.snap_id = ?",
                                  ri->object_id, "x", "x", target) == 0;
        }
        if (!target && !why[0]) snprintf(why, sizeof(why), "no snapshot holds the newer version");
        if (certain) snprintf(why, sizeof(why), "current version is a known ancestor of the newer one");

        /* One open offer per save object: older ones are superseded. */
        sqlite3_stmt *st = NULL;
        if (g_ls_db && sqlite3_prepare_v2(g_ls_db, "UPDATE recovery SET state = 'superseded', resolved_at = ? WHERE title_id = ?"
                                          " AND user_id = ? AND object_id = ? AND state = 'open'", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)now);
            ls_bind_text(st, 2, title_id);
            ls_bind_text(st, 3, ri->user_id);
            ls_bind_text(st, 4, ri->object_id);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
        int64_t id = 0;
        st = NULL;
        if (g_ls_db && sqlite3_prepare_v2(g_ls_db, "INSERT INTO recovery(user_id, title_id, object_id, detected_at, reason,"
                                          " target_snap, files, certain, target_on_ps5, target_on_pc, state, detail)"
                                          " VALUES(?,?,?,?,?,?,?,?,?,?,'open',?)", -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, ri->user_id);
            ls_bind_text(st, 2, title_id);
            ls_bind_text(st, 3, ri->object_id);
            sqlite3_bind_int64(st, 4, (sqlite3_int64)now);
            ls_bind_text(st, 5, reason);
            sqlite3_bind_int64(st, 6, (sqlite3_int64)target);
            ls_bind_text(st, 7, files);
            sqlite3_bind_int(st, 8, certain);
            sqlite3_bind_int(st, 9, on_ps5);
            sqlite3_bind_int(st, 10, pc);
            ls_bind_text(st, 11, why);
            if (sqlite3_step(st) == SQLITE_DONE) id = sqlite3_last_insert_rowid(g_ls_db);
        }
        sqlite3_finalize(st);
        pthread_mutex_unlock(&g_ls_mtx);
        if (!id) continue;

        const char *gname = lookup_title(title_id);
        if (!gname || !gname[0]) gname = title_id;
        ls_event("REGRESSION_DETECTED", title_id, "#%lld save %s went back to an older version (%s); newer version in snapshot #%lld;"
                 " %s: %s", (long long)id, ri->object_id, files, (long long)target, certain ? "certain" : "not certain", why);
        if (strcmp(cfg.regression_recovery, "off") == 0) continue;
        if (strcmp(cfg.regression_recovery, "certain") == 0 && certain) {
            char err[300] = {0};
            int rc = ls_restore_request_snapshot(target, ri->object_id, err, sizeof(err));
            pthread_mutex_lock(&g_ls_mtx);
            st = NULL;
            if (g_ls_db && sqlite3_prepare_v2(g_ls_db, "UPDATE recovery SET state = ?, resolved_at = ?, detail = ? WHERE id = ?",
                                              -1, &st, NULL) == SQLITE_OK) {
                ls_bind_text(st, 1, rc == 0 ? "auto_restore" : "open");
                sqlite3_bind_int64(st, 2, rc == 0 ? (sqlite3_int64)now : 0);
                char d[400];
                snprintf(d, sizeof(d), rc == 0 ? "restoring the newer version automatically" : "automatic restore not possible: %s", err);
                ls_bind_text(st, 3, d);
                sqlite3_bind_int64(st, 4, (sqlite3_int64)id);
                sqlite3_step(st);
            }
            sqlite3_finalize(st);
            pthread_mutex_unlock(&g_ls_mtx);
            if (rc == 0) {
                ls_event("REGRESSION_AUTO_RESTORE", title_id, "#%lld save %s: restoring snapshot #%lld", (long long)id,
                         ri->object_id, (long long)target);
                notify("Shallot SaveMgr: save %s of %s went back to an older version; restoring the newer one", ri->object_id, gname);
                continue;
            }
            ls_event("REGRESSION_AUTO_RESTORE_REFUSED", title_id, "#%lld save %s: %s", (long long)id, ri->object_id, err);
        }
        notify("Shallot SaveMgr: save %s of %s went back to an older version. Open Backup & Sync to restore the newer one",
               ri->object_id, gname);
    }
}

/* Owner decision on an offer. action: "restore" (B) or "keep" (A). */
static int ls_recovery_resolve(int64_t id, const char *action, char *err, size_t err_sz) {
    char state[24] = {0}, obj[256] = {0}, title[32] = {0};
    int64_t target = 0;
    int found = 0;
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *st = NULL;
    if (g_ls_db && g_ls_schema >= 7 &&
        sqlite3_prepare_v2(g_ls_db, "SELECT state, object_id, title_id, target_snap FROM recovery WHERE id = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            found = 1;
            ls_col_text(st, 0, state, sizeof(state));
            ls_col_text(st, 1, obj, sizeof(obj));
            ls_col_text(st, 2, title, sizeof(title));
            target = sqlite3_column_int64(st, 3);
        }
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    if (!found) { snprintf(err, err_sz, "no recovery offer #%lld", (long long)id); return -1; }
    if (strcmp(state, "open") != 0) { snprintf(err, err_sz, "offer #%lld is already %s", (long long)id, state); return -1; }
    const char *new_state;
    char detail[300] = {0};
    if (strcmp(action, "keep") == 0) {
        new_state = "kept";
        snprintf(detail, sizeof(detail), "kept the current version");
    } else if (strcmp(action, "restore") == 0) {
        if (!target) { snprintf(err, err_sz, "no snapshot holds the newer version; use Versions / Restore"); return -1; }
        int rc = ls_restore_request_snapshot(target, obj, err, err_sz);
        if (rc < 0) return rc;
        new_state = "restored";
        snprintf(detail, sizeof(detail), "restore of snapshot #%lld requested", (long long)target);
    } else {
        snprintf(err, err_sz, "unknown action");
        return -1;
    }
    pthread_mutex_lock(&g_ls_mtx);
    st = NULL;
    if (g_ls_db && sqlite3_prepare_v2(g_ls_db, "UPDATE recovery SET state = ?, resolved_at = ?, detail = ? WHERE id = ?",
                                      -1, &st, NULL) == SQLITE_OK) {
        ls_bind_text(st, 1, new_state);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)time(NULL));
        ls_bind_text(st, 3, detail);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    ls_event("REGRESSION_RESOLVED", title, "#%lld save %s: %s", (long long)id, obj, detail);
    return 0;
}

static int64_t ls_recovery_open_count(const char *title_id) {
    int64_t n = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db && g_ls_schema >= 7)
        n = title_id && title_id[0]
            ? ls_db_int_locked("SELECT COUNT(*) FROM recovery WHERE state = 'open' AND title_id = ?", title_id, NULL, NULL, 0)
            : ls_db_int_locked("SELECT COUNT(*) FROM recovery WHERE state = 'open'", NULL, NULL, NULL, 0);
    pthread_mutex_unlock(&g_ls_mtx);
    return n;
}

/* Offers (open first, newest first), optionally for one title. */
static void ls_recovery_json(const char *title_id, char **buf, size_t *cap, size_t *pos) {
    ls_json_append(buf, cap, pos, "{\"offers\":[");
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *st = NULL;
    if (g_ls_db && g_ls_schema >= 7 &&
        sqlite3_prepare_v2(g_ls_db, "SELECT id, user_id, title_id, object_id, detected_at, reason, target_snap, files, certain,"
                           " target_on_ps5, target_on_pc, state, stale, resolved_at, detail,"
                           " COALESCE((SELECT created_at FROM snapshots s WHERE s.snap_id = recovery.target_snap), 0)"
                           " FROM recovery WHERE (? = '' OR title_id = ?)"
                           " ORDER BY (state = 'open') DESC, id DESC LIMIT 100", -1, &st, NULL) == SQLITE_OK) {
        ls_bind_text(st, 1, title_id);
        ls_bind_text(st, 2, title_id);
        int first = 1;
        while (sqlite3_step(st) == SQLITE_ROW) {
            char f[16][620];
            int txt[] = { 1, 2, 3, 5, 7, 11, 14 };
            for (int k = 0; k < 7; k++) {
                char raw[600];
                ls_col_text(st, txt[k], raw, sizeof(raw));
                json_escape(raw, f[txt[k]], sizeof(f[txt[k]]));
            }
            const char *name = lookup_title(f[2]);
            char en[520];
            json_escape(name ? name : "", en, sizeof(en));
            ls_json_append(buf, cap, pos,
                "%s{\"id\":%lld,\"user_id\":\"%s\",\"title_id\":\"%s\",\"name\":\"%s\",\"object_id\":\"%s\",\"detected_at\":%lld,"
                "\"reason\":\"%s\",\"target_snap\":%lld,\"target_created_at\":%lld,\"files\":\"%s\",\"certain\":%s,"
                "\"target_on_ps5\":%s,\"target_on_pc\":%s,\"state\":\"%s\",\"stale\":%s,\"resolved_at\":%lld,\"detail\":\"%s\"}",
                first ? "" : ",", (long long)sqlite3_column_int64(st, 0), f[1], f[2], en, f[3],
                (long long)sqlite3_column_int64(st, 4), f[5], (long long)sqlite3_column_int64(st, 6),
                (long long)sqlite3_column_int64(st, 15), f[7], sqlite3_column_int(st, 8) ? "true" : "false",
                sqlite3_column_int(st, 9) ? "true" : "false", sqlite3_column_int(st, 10) ? "true" : "false", f[11],
                sqlite3_column_int(st, 12) ? "true" : "false", (long long)sqlite3_column_int64(st, 13), f[14]);
            first = 0;
        }
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    ls_json_append(buf, cap, pos, "]}");
}
