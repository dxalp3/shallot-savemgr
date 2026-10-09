/*
 * Host tests for regression recovery (stage N): a save that went back to an
 * older archived version yields one offer per save object, certainty rules,
 * the off / ask / certain policies, Restore B / Keep A, deliberate restores
 * never counting as regressions, stale offers and superseding.
 */
#include "host_worker_stubs.h"

#define T "PPSA00002"
#define U "10000000"
#define D LS_USER_HOME "/" U "/savedata_prospero/" T

static int64_t q_int(const char *sql) {
    sqlite3_stmt *st = NULL; int64_t v = -1;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    return v;
}
static void q_text(const char *sql, char *out, size_t n) {
    sqlite3_stmt *st = NULL; out[0] = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        ls_col_text(st, 0, out, n);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
}
static void put(const char *path, const char *data) {
    write_file(path, data);
    struct timespec ts[2];
    clock_gettime(CLOCK_REALTIME, &ts[0]);
    ts[0].tv_sec += 50 + rand() % 900; ts[1] = ts[0];
    utimensat(AT_FDCWD, path, ts, 0);
}
static int holds(const char *path, const char *want) {
    char b[256] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t k = fread(b, 1, sizeof(b) - 1, f); b[k] = 0; fclose(f);
    return strcmp(b, want) == 0;
}
static void drop_jobs(void) {
    pthread_mutex_lock(&g_ls_job_mtx);
    while (g_ls_jobs) { ls_job_t *j = g_ls_jobs; g_ls_jobs = j->next; free(j); }
    pthread_mutex_unlock(&g_ls_job_mtx);
}
static void policy(const char *p) {
    ls_config_t c;
    ls_load_config(&c);
    snprintf(c.regression_recovery, sizeof(c.regression_recovery), "%s", p);
    ls_save_config(&c);
}
static int64_t newest_offer(void) { return q_int("SELECT COALESCE(MAX(id), 0) FROM recovery"); }

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    CHECK(g_ls_schema >= 7, "schema v7");
    g_ls_worker_started = 1;
    g_running_title = T;
    g_test_sfo_dir_from_path = 1;
    memset(&g_test_sfo, 0, sizeof(g_test_sfo));
    snprintf(g_test_sfo.title_id, sizeof(g_test_sfo.title_id), T);
    g_test_sfo.account_id = 0x1111;
    char *iobuf = malloc(BUF_SIZE), st[32], err[300];
    policy("ask");

    mkdirs(D);
    put(D "/sdimg_Save04", "A");
    put(D "/sdimg_sce_bu_Save04", "A-bu");
    put(D "/sdimg_Save05", "five");
    ls_protect_title(T, "game_close", iobuf);                 /* gen A */
    put(D "/sdimg_Save04", "B");
    put(D "/sdimg_sce_bu_Save04", "B-bu");
    int64_t snapB = 0;
    ls_protect_title_ex(T, "game_close", iobuf, &snapB);       /* gen B, snapshot holds B */
    CHECK(q_int("SELECT COUNT(*) FROM recovery") == 0, "no offer while saves only move forward");

    /* 1. Panic: Save04 and its copy are back at A. */
    put(D "/sdimg_Save04", "A");
    put(D "/sdimg_sce_bu_Save04", "A-bu");
    ls_protect_title(T, "startup_reconcile", iobuf);
    CHECK(q_int("SELECT COUNT(*) FROM recovery WHERE state = 'open'") == 1, "one offer for the save object (not per file)");
    q_text("SELECT object_id FROM recovery", st, sizeof(st));
    CHECK(strcmp(st, "Save04") == 0, "offer names the object (%s)", st);
    char sql[200];
    snprintf(sql, sizeof(sql), "SELECT target_snap = %lld AND certain = 1 AND target_on_ps5 = 1 FROM recovery", (long long)snapB);
    CHECK(q_int(sql) == 1, "certain, target = snapshot with B, archives on the PS5");
    CHECK(ev_count_code("REGRESSION_DETECTED") == 1, "event");
    CHECK(ls_recovery_open_count(T) == 1, "open count");
    CHECK(holds(D "/sdimg_Save04", "A"), "ask: nothing restored automatically");

    /* 2. Restore B. */
    int64_t id = newest_offer();
    CHECK(ls_recovery_resolve(id, "restore", err, sizeof(err)) == 0, "restore accepted (%s)", err);
    drop_jobs(); ls_restore_run(iobuf);
    CHECK(holds(D "/sdimg_Save04", "B") && holds(D "/sdimg_sce_bu_Save04", "B-bu"), "save and its copy back at B");
    CHECK(holds(D "/sdimg_Save05", "five"), "sibling untouched");
    CHECK(q_int("SELECT COUNT(*) FROM recovery WHERE state = 'restored'") == 1, "offer resolved");
    CHECK(ls_recovery_resolve(id, "keep", err, sizeof(err)) < 0, "resolved offers cannot be resolved again");
    ls_protect_title(T, "game_close", iobuf);
    CHECK(q_int("SELECT COUNT(*) FROM recovery WHERE state = 'open'") == 0, "restoring B creates no new offer");

    /* 3. Deliberate restore of an older version is not a regression. */
    {
        int64_t snapA = q_int("SELECT MIN(snap_id) FROM snapshots WHERE kind = 'title'");
        CHECK(ls_restore_request_snapshot(snapA, "Save04", err, sizeof(err)) == 0, "deliberate restore (%s)", err);
        drop_jobs(); ls_restore_run(iobuf);
        CHECK(holds(D "/sdimg_Save04", "A"), "older version restored on purpose");
        ls_protect_title(T, "game_close", iobuf);
        ls_protect_title(T, "idle_sweep", iobuf);
        CHECK(q_int("SELECT COUNT(*) FROM recovery WHERE state = 'open'") == 0, "no offer after a deliberate restore");
    }

    /* 4. Keep A. Then moving forward and regressing again creates a fresh offer. */
    put(D "/sdimg_Save04", "C");
    ls_protect_title(T, "game_close", iobuf);
    put(D "/sdimg_Save04", "A");
    ls_protect_title(T, "game_close", iobuf);
    id = newest_offer();
    CHECK(q_int("SELECT COUNT(*) FROM recovery WHERE state = 'open'") == 1, "new offer after C -> A");
    CHECK(ls_recovery_resolve(id, "keep", err, sizeof(err)) == 0 && holds(D "/sdimg_Save04", "A"), "keep: nothing changes");
    CHECK(q_int("SELECT COUNT(*) FROM recovery WHERE state = 'kept'") == 1, "kept");

    /* 5. Not certain: the regressed file's version is not an ancestor of the target (divergent). */
    put(D "/sdimg_Save04", "B");                 /* known gen B, whose parent chain does not contain C */
    ls_protect_title(T, "game_close", iobuf);    /* newest gen is C: B < C, B not an ancestor of C */
    id = newest_offer();
    snprintf(sql, sizeof(sql), "SELECT certain FROM recovery WHERE id = %lld", (long long)id);
    CHECK(q_int("SELECT COUNT(*) FROM recovery WHERE state = 'open'") == 1 && q_int(sql) == 0, "divergent history: not certain");

    /* 6. Stale: the save gets new content while an offer is open. */
    put(D "/sdimg_Save04", "D");
    ls_protect_title(T, "game_close", iobuf);
    snprintf(sql, sizeof(sql), "SELECT stale FROM recovery WHERE id = %lld", (long long)id);
    CHECK(q_int(sql) == 1, "offer flagged stale after new content");

    /* 7. Superseded: a newer regression replaces the open offer. */
    put(D "/sdimg_Save04", "C");
    ls_protect_title(T, "game_close", iobuf);
    snprintf(sql, sizeof(sql), "SELECT state FROM recovery WHERE id = %lld", (long long)id);
    q_text(sql, st, sizeof(st));
    CHECK(strcmp(st, "superseded") == 0, "older offer superseded (%s)", st);
    CHECK(q_int("SELECT COUNT(*) FROM recovery WHERE state = 'open'") == 1, "still one open offer per object");
    pthread_mutex_lock(&g_ls_mtx); ls_exec_locked("UPDATE recovery SET state = 'kept' WHERE state = 'open'"); pthread_mutex_unlock(&g_ls_mtx);

    /* 8. Policy certain: certain offers restore automatically. */
    policy("certain");
    put(D "/sdimg_Save04", "E");
    ls_protect_title(T, "game_close", iobuf);    /* E, parent D... chain contains C? -> E's parent is C */
    put(D "/sdimg_Save04", "C");                 /* back to its direct ancestor */
    drop_jobs();
    ls_protect_title(T, "game_close", iobuf);
    q_text("SELECT state FROM recovery ORDER BY id DESC LIMIT 1", st, sizeof(st));
    CHECK(strcmp(st, "auto_restore") == 0 && ev_count_code("REGRESSION_AUTO_RESTORE") == 1, "auto restore requested (%s)", st);
    ls_restore_run(iobuf);
    CHECK(holds(D "/sdimg_Save04", "E"), "newer version back automatically");

    /* 9. Policy certain, but the newer version is only on the PC: asks instead. */
    put(D "/sdimg_Save04", "F");
    ls_protect_title(T, "game_close", iobuf);
    pthread_mutex_lock(&g_ls_mtx);
    ls_exec_locked("UPDATE queue SET state = 'pruned' WHERE id = (SELECT g.queue_id FROM generations g WHERE g.save_name = 'sdimg_Save04'"
                   " ORDER BY g.gen_id DESC LIMIT 1)");
    pthread_mutex_unlock(&g_ls_mtx);
    put(D "/sdimg_Save04", "E");
    drop_jobs();
    ls_protect_title(T, "game_close", iobuf);
    q_text("SELECT state || '/' || certain || '/' || target_on_ps5 FROM recovery ORDER BY id DESC LIMIT 1", st, sizeof(st));
    CHECK(strcmp(st, "open/0/0") == 0, "PC-only target: not certain, left for the owner (%s)", st);
    CHECK(holds(D "/sdimg_Save04", "E"), "nothing restored");

    /* 10. Policy off: recorded, never acted on. */
    policy("off");
    pthread_mutex_lock(&g_ls_mtx); ls_exec_locked("UPDATE recovery SET state = 'kept' WHERE state = 'open'"); pthread_mutex_unlock(&g_ls_mtx);
    put(D "/sdimg_Save05", "five-2");
    ls_protect_title(T, "game_close", iobuf);
    put(D "/sdimg_Save05", "five");
    drop_jobs();
    ls_protect_title(T, "game_close", iobuf);
    CHECK(q_int("SELECT COUNT(*) FROM recovery WHERE state = 'open' AND object_id = 'Save05'") == 1, "off: offer recorded");
    CHECK(g_ls_jobs == NULL && holds(D "/sdimg_Save05", "five"), "off: no restore queued");

    /* 11. Test aid: a simulated regression produces an offer like a crash would. */
    policy("ask");
    pthread_mutex_lock(&g_ls_mtx); ls_exec_locked("UPDATE recovery SET state = 'kept' WHERE state = 'open'"); pthread_mutex_unlock(&g_ls_mtx);
    {
        int64_t first = q_int("SELECT MIN(snap_id) FROM snapshots WHERE kind = 'title'");
        int64_t before = newest_offer();
        CHECK(ls_restore_request_simulated_regression(first, "Save04", err, sizeof(err)) == 0, "simulation requested (%s)", err);
        drop_jobs(); ls_restore_run(iobuf);
        CHECK(holds(D "/sdimg_Save04", "A"), "older version placed");
        CHECK(newest_offer() > before && q_int("SELECT COUNT(*) FROM recovery WHERE state = 'open' AND object_id = 'Save04'") == 1,
              "simulated regression yields an offer");
        CHECK(ls_restore_request_simulated_regression(first, "", err, sizeof(err)) < 0, "object required");
    }

    free(iobuf);
    return host_finish();
}
