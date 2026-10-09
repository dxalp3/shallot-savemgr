/*
 * Host tests for snapshot restores: a whole game restored as one unit, one
 * save object restored without touching its siblings, a failure halfway
 * through undoing every file already placed, legacy (migrated) snapshots,
 * archives no longer on the PS5, and orphan backup copies.
 */
#include "host_worker_stubs.h"

#define T   "PPSA00002"
#define U   "10000000"
#define D   LS_USER_HOME "/" U "/savedata_prospero/" T
#define DBD LS_SYSTEM_DATA "/savedata_prospero/" U "/db/user"

static int64_t q_int(const char *sql) {
    sqlite3_stmt *st = NULL; int64_t v = -1;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    return v;
}
static void put(const char *path, const char *data) {
    write_file(path, data);
    struct timespec ts[2];
    clock_gettime(CLOCK_REALTIME, &ts[0]);
    ts[0].tv_sec += 50 + rand() % 500; ts[1] = ts[0];
    utimensat(AT_FDCWD, path, ts, 0);
}
static int holds(const char *path, const char *want) {
    char b[256] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return want == NULL;
    size_t k = fread(b, 1, sizeof(b) - 1, f); b[k] = 0; fclose(f);
    return want && strcmp(b, want) == 0;
}
static void drop_jobs(void) {
    pthread_mutex_lock(&g_ls_job_mtx);
    while (g_ls_jobs) { ls_job_t *j = g_ls_jobs; g_ls_jobs = j->next; free(j); }
    pthread_mutex_unlock(&g_ls_job_mtx);
}
static int run_snapshot(int64_t snap, const char *obj, char *iobuf, char *err, size_t en) {
    int rc = ls_restore_request_snapshot(snap, obj, err, en);
    if (rc == 0) { drop_jobs(); ls_restore_run(iobuf); }
    return rc;
}
static int preview(int64_t snap, char *fp, char *list, size_t ln) {
    size_t cap = 4096, pos = 0;
    char *b = malloc(cap); b[0] = 0;
    char err[300];
    int rc = ls_restore_preview_json(snap, &b, &cap, &pos, err, sizeof(err));
    fp[0] = 0;
    char *f = strstr(b, "\"remove_fp\":\"");
    if (f) { memcpy(fp, f + 13, 64); fp[64] = 0; }
    snprintf(list, ln, "%s", b);
    free(b);
    return rc;
}
static int run_exact(int64_t snap, const char *fp, char *iobuf, char *err, size_t en) {
    int rc = ls_restore_request_snapshot_ex(snap, NULL, 1, fp, err, en);
    if (rc == 0) { drop_jobs(); ls_restore_run(iobuf); }
    return rc;
}
static int state_is(const char *st) {
    pthread_mutex_lock(&g_ls_restore_mtx);
    int r = strcmp(g_ls_restore.state, st) == 0;
    pthread_mutex_unlock(&g_ls_restore_mtx);
    return r;
}
static const char *item_result(const char *save) {
    static char res[16];
    res[0] = 0;
    pthread_mutex_lock(&g_ls_restore_mtx);
    for (int i = 0; i < g_ls_restore.nitems; i++)
        if (strcmp(g_ls_restore.items[i].save_name, save) == 0) snprintf(res, sizeof(res), "%s", g_ls_restore.items[i].result);
    pthread_mutex_unlock(&g_ls_restore_mtx);
    return res;
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    g_ls_worker_started = 1;
    g_running_title = T;
    char *iobuf = malloc(BUF_SIZE);
    char err[300];

    mkdirs(DBD);
    sqlite3 *db = NULL;
    sqlite3_open(DBD "/savedata.db", &db);
    sqlite3_exec(db, "CREATE TABLE savedata(id INTEGER PRIMARY KEY AUTOINCREMENT, title_id, dir_name, main_title, sub_title,"
                     " detail, tmp_dir_name, is_broken, user_param, blocks, free_blocks, size_kib, mtime, fake_broken,"
                     " account_id, user_id, faked_owner, cloud_icon_url, cloud_revision, game_title_id, system_blocks);",
                 NULL, NULL, NULL);
    sqlite3_close(db);
    memset(&g_test_sfo, 0, sizeof(g_test_sfo));
    snprintf(g_test_sfo.title_id, sizeof(g_test_sfo.title_id), T);
    g_test_sfo.account_id = 0x1111;

    mkdirs(D);
    put(D "/sdimg_Save00", "s0-v1");
    put(D "/sdimg_sce_bu_Save00", "s0-v1");
    put(D "/sdimg_Save04", "s4-v1");
    put(D "/sdimg_sce_bu_Save04", "s4-v1");
    put(D "/sdimg_Save05", "s5-v1");
    int64_t snap1 = 0, snap2 = 0;
    ls_protect_title_ex(T, "game_close", iobuf, &snap1);
    put(D "/sdimg_Save04", "s4-v2");
    put(D "/sdimg_sce_bu_Save04", "s4-v2");
    put(D "/sdimg_Save05", "s5-v2");
    ls_protect_title_ex(T, "game_close", iobuf, &snap2);
    CHECK(snap1 > 0 && snap2 > snap1, "two game snapshots (%lld, %lld)", (long long)snap1, (long long)snap2);
    put(D "/sdimg_Save09", "s9-new");             /* exists now, not in either snapshot */

    /* param.sfo naming another directory: the whole restore is refused before any change. */
    snprintf(g_test_sfo.dir_name, sizeof(g_test_sfo.dir_name), "OTHER");
    CHECK(run_snapshot(snap1, NULL, iobuf, err, sizeof(err)) == 0, "request (%s)", err);
    CHECK(state_is("failed") && strstr(g_ls_restore.message, "save directory"), "param.sfo dir mismatch refused (%s)",
          g_ls_restore.message);
    CHECK(holds(D "/sdimg_Save04", "s4-v2"), "refusal changed nothing");

    /* From here on the stub reports the directory of whatever was mounted. */
    g_test_sfo_dir_from_path = 1;

    /* 6. Whole game: every object of snapshot 1 restored as a unit. */
    CHECK(run_snapshot(snap1, NULL, iobuf, err, sizeof(err)) == 0, "game restore requested (%s)", err);
    CHECK(state_is("done"), "game restore done (%s)", g_ls_restore.message);
    CHECK(holds(D "/sdimg_Save04", "s4-v1") && holds(D "/sdimg_sce_bu_Save04", "s4-v1") && holds(D "/sdimg_Save05", "s5-v1"),
          "Save04 pair and Save05 back to v1");
    CHECK(holds(D "/sdimg_Save00", "s0-v1") && strcmp(item_result("sdimg_Save00"), "unchanged") == 0, "Save00 untouched (unchanged)");
    CHECK(holds(D "/sdimg_Save09", "s9-new") && g_ls_restore.kept_objects == 1 && ev_count_code("RESTORE_KEPT") == 1,
          "Save09 (not in the snapshot) left as it is");
    CHECK(strstr(g_ls_restore.message, "save object(s)") != NULL, "message at game level (%s)", g_ls_restore.message);
    CHECK(g_ls_restore.rollback_snap > snap2, "pre-restore game snapshot recorded (#%lld)", (long long)g_ls_restore.rollback_snap);

    /* 7. One object: Save04 from snapshot 2; Save05 (also different in snapshot 2) untouched. */
    CHECK(run_snapshot(snap2, "Save04", iobuf, err, sizeof(err)) == 0, "object restore requested (%s)", err);
    CHECK(state_is("done") && strcmp(g_ls_restore.kind, "object") == 0, "object restore done (%s)", g_ls_restore.message);
    CHECK(holds(D "/sdimg_Save04", "s4-v2") && holds(D "/sdimg_sce_bu_Save04", "s4-v2"), "Save04 pair restored together");
    CHECK(holds(D "/sdimg_Save05", "s5-v1") && holds(D "/sdimg_Save00", "s0-v1") && holds(D "/sdimg_Save09", "s9-new"),
          "sibling objects untouched");
    CHECK(g_ls_restore.nitems == 2, "only the two Save04 files were items (%d)", g_ls_restore.nitems);

    /* 10. Failure halfway: Save05's slot cannot be written; Save04 already placed is undone. */
    int64_t back = g_ls_restore.rollback_snap;
    (void)back;
    unlink(D "/sdimg_Save05");
    mkdirs(D "/sdimg_Save05");                    /* rename over a directory fails */
    int64_t verified0 = ev_count_code("RESTORE_VERIFIED");
    CHECK(run_snapshot(snap1, NULL, iobuf, err, sizeof(err)) == 0, "game restore requested");
    CHECK(state_is("failed"), "not reported as success (%s)", g_ls_restore.state);
    CHECK(strstr(g_ls_restore.message, "sdimg_Save05") && strstr(g_ls_restore.message, "undone"),
          "per-file error and undo reported (%s)", g_ls_restore.message);
    CHECK(holds(D "/sdimg_Save04", "s4-v2") && holds(D "/sdimg_sce_bu_Save04", "s4-v2"), "Save04 pair put back to what it was");
    CHECK(strcmp(item_result("sdimg_Save04"), "undone") == 0 && strcmp(item_result("sdimg_Save05"), "failed") == 0,
          "per-item results (%s / %s)", item_result("sdimg_Save04"), item_result("sdimg_Save05"));
    CHECK(ev_count_code("RESTORE_VERIFIED") == verified0, "nothing recorded as restored");
    rmdir(D "/sdimg_Save05");
    put(D "/sdimg_Save05", "s5-v1");

    /* Archive gone from the PS5: refused up front, with the file named. */
    {
        char sql[300];
        snprintf(sql, sizeof(sql), "UPDATE queue SET state = 'pruned' WHERE id = (SELECT g.queue_id FROM snapshot_files f"
                 " JOIN generations g ON g.gen_id = f.gen_id WHERE f.snap_id = %lld AND f.save_name = 'sdimg_Save05')", (long long)snap2);
        pthread_mutex_lock(&g_ls_mtx); ls_exec_locked(sql); pthread_mutex_unlock(&g_ls_mtx);
        CHECK(run_snapshot(snap2, NULL, iobuf, err, sizeof(err)) == -7 && strstr(err, "sdimg_Save05"), "missing archive refused (%s)", err);
        CHECK(run_snapshot(snap2, "Save04", iobuf, err, sizeof(err)) == 0 && state_is("done"), "other objects still restorable");
        CHECK(!g_ls_rjob.active, "slot released");
    }

    /* Orphan backup copy in a snapshot is not written next to an existing primary. */
    {
        put(D "/sdimg_sce_bu_Save08", "s8-bu");
        int64_t s3 = 0;
        ls_protect_title_ex(T, "game_close", iobuf, &s3);
        put(D "/sdimg_Save08", "s8-primary-now");
        put(D "/sdimg_sce_bu_Save08", "s8-bu-v2");
        CHECK(run_snapshot(s3, "Save08", iobuf, err, sizeof(err)) == 0, "orphan object request");
        CHECK(state_is("failed") && strstr(g_ls_restore.message, "skipped"), "orphan copy skipped (%s)", g_ls_restore.message);
        CHECK(holds(D "/sdimg_sce_bu_Save08", "s8-bu-v2") && holds(D "/sdimg_Save08", "s8-primary-now"), "nothing written");
    }

    /* Exact game restore: files not in the snapshot are removed only after an explicit, bound confirmation. */
    {
        char fp[80], list[4096];
        db = NULL; sqlite3_open(DBD "/savedata.db", &db);
        sqlite3_exec(db, "INSERT INTO savedata(title_id, dir_name, mtime) VALUES('" T "','Save09','x'),('" T "','Save08','x');",
                     NULL, NULL, NULL);
        sqlite3_close(db);
        CHECK(preview(snap1, fp, list, sizeof(list)) == 0 && strlen(fp) == 64, "preview (%s)", list);
        CHECK(strstr(list, "\"object_id\":\"Save09\"") && strstr(list, "\"object_id\":\"Save08\",\"files\":[\"sdimg_Save08\",\"sdimg_sce_bu_Save08\"]")
              && strstr(list, "\"remove_files\":3"), "removal list names Save09 and the Save08 pair (%s)", list);
        CHECK(!strstr(list, "Save04\",\"files"), "saves in the snapshot are not listed for removal");
        /* Confirmation must match the current list. */
        CHECK(run_exact(snap1, "0000000000000000000000000000000000000000000000000000000000000000", iobuf, err, sizeof(err)) == -8,
              "stale confirmation refused (%s)", err);
        CHECK(ls_restore_request_snapshot_ex(snap1, "Save04", 1, fp, err, sizeof(err)) < 0, "exact only for a whole game");
        CHECK(!g_ls_rjob.active, "slot not taken by refusals");
        /* A placement failure removes nothing. */
        unlink(D "/sdimg_Save04");
        mkdirs(D "/sdimg_Save04");
        preview(snap1, fp, list, sizeof(list));
        CHECK(run_exact(snap1, fp, iobuf, err, sizeof(err)) == 0 && state_is("failed"), "placement failure (%s)", g_ls_restore.message);
        CHECK(holds(D "/sdimg_Save09", "s9-new") && holds(D "/sdimg_Save08", "s8-primary-now"), "nothing removed after a failure");
        rmdir(D "/sdimg_Save04");
        put(D "/sdimg_Save04", "s4-v2");
        /* Success. */
        preview(snap1, fp, list, sizeof(list));
        CHECK(run_exact(snap1, fp, iobuf, err, sizeof(err)) == 0 && state_is("done"), "exact restore done (%s)", g_ls_restore.message);
        CHECK(!file_exists(D "/sdimg_Save09") && !file_exists(D "/sdimg_Save08") && !file_exists(D "/sdimg_sce_bu_Save08"),
              "files not in the snapshot removed");
        CHECK(holds(D "/sdimg_Save04", "s4-v1") && holds(D "/sdimg_Save05", "s5-v1"), "snapshot contents placed");
        CHECK(g_ls_restore.removed_objects == 2 && ev_count_code("RESTORE_REMOVED") == 3, "removed 2 saves / 3 files (%d)",
              g_ls_restore.removed_objects);
        CHECK(strcmp(item_result("sdimg_Save09"), "removed") == 0, "per-item result removed");
        CHECK(q_int("SELECT COUNT(*) FROM queue WHERE save_name = 'sdimg_Save08' AND reason = 'pre_restore'") >= 1,
              "the never-backed-up Save08 primary was archived before removal");
        CHECK(q_int("SELECT COUNT(*) FROM saves WHERE save_name = 'sdimg_Save09' AND state = 'missing'") == 1, "bookkeeping: missing");
        {
            sqlite3 *sdb = NULL; sqlite3_stmt *st = NULL; int rows = -1;
            sqlite3_open(DBD "/savedata.db", &sdb);
            if (sqlite3_prepare_v2(sdb, "SELECT COUNT(*) FROM savedata WHERE dir_name IN ('Save09','Save08')", -1, &st, NULL) == SQLITE_OK &&
                sqlite3_step(st) == SQLITE_ROW) rows = sqlite3_column_int(st, 0);
            sqlite3_finalize(st); sqlite3_close(sdb);
            CHECK(rows == 0, "savedata.db rows of removed saves deleted (%d)", rows);
        }
        /* Everything removed comes back from the pre-restore game snapshot. */
        int64_t back = g_ls_restore.rollback_snap;
        CHECK(run_snapshot(back, NULL, iobuf, err, sizeof(err)) == 0 && state_is("done"), "undo via pre-restore snapshot (%s)",
              g_ls_restore.message);
        CHECK(holds(D "/sdimg_Save09", "s9-new") && holds(D "/sdimg_Save08", "s8-primary-now") &&
              holds(D "/sdimg_sce_bu_Save08", "s8-bu-v2") && holds(D "/sdimg_Save04", "s4-v2"), "removed saves recovered");
    }

    /* Deleting one save: both files go, backed up first, and it comes back from a snapshot. */
    {
        CHECK(ls_restore_request_delete("", T, "Save04", err, sizeof(err)) == 0, "delete requested (%s)", err);
        drop_jobs(); ls_restore_run(iobuf);
        CHECK(state_is("done") && strcmp(g_ls_restore.kind, "delete") == 0 && strstr(g_ls_restore.message, "deleted save Save04"),
              "delete done (%s)", g_ls_restore.message);
        CHECK(!file_exists(D "/sdimg_Save04") && !file_exists(D "/sdimg_sce_bu_Save04"), "save file and backup copy removed");
        CHECK(holds(D "/sdimg_Save00", "s0-v1") && holds(D "/sdimg_Save05", "s5-v1"), "other saves untouched");
        {
            char sql[200];
            snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM snapshot_files WHERE snap_id = %lld AND object_id = 'Save04'",
                     (long long)g_ls_restore.rollback_snap);
            CHECK(g_ls_restore.rollback_snap > 0 && q_int(sql) == 2, "the game snapshot named in the result holds the deleted pair");
        }
        CHECK(ls_restore_request_delete("", T, "Save04", err, sizeof(err)) < 0 && strstr(err, "not found"), "already gone (%s)", err);
        CHECK(ls_restore_request_delete("", T, "../x", err, sizeof(err)) < 0, "unknown save refused");
        CHECK(run_snapshot(snap2, "Save04", iobuf, err, sizeof(err)) == 0 && state_is("done"), "restored after delete (%s)",
              g_ls_restore.message);
        CHECK(holds(D "/sdimg_Save04", "s4-v2") && holds(D "/sdimg_sce_bu_Save04", "s4-v2"), "pair re-created");
    }

    /* 8. Legacy: the migrated snapshot of a pre-v6 database restores as a set. */
    {
        pthread_mutex_lock(&g_ls_mtx);
        ls_exec_locked("DELETE FROM snapshot_files; DELETE FROM snapshots;");
        ls_exec_locked(ls_schema_v6);       /* what the migration does */
        pthread_mutex_unlock(&g_ls_mtx);
        int64_t mig = q_int("SELECT MAX(snap_id) FROM snapshots WHERE reason = 'migrated'");
        CHECK(mig > 0 && q_int("SELECT objects FROM snapshots WHERE reason = 'migrated'") >= 4, "migrated snapshot with objects");
        put(D "/sdimg_Save04", "s4-local-edit");
        CHECK(run_snapshot(mig, NULL, iobuf, err, sizeof(err)) == 0 && state_is("done"), "migrated snapshot restored (%s)",
              g_ls_restore.message);
        CHECK(holds(D "/sdimg_Save04", "s4-v2"), "Save04 back to its last protected state");
        CHECK(q_int("SELECT COUNT(*) FROM queue WHERE reason = 'pre_restore'") >= 1, "local edit archived before the restore");
    }

    free(iobuf);
    return host_finish();
}
