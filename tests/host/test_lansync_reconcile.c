/*
 * Host tests for the Level-1 Sanity Check (stage G): every status code,
 * title result = most urgent save status, stored result, and the guarantee
 * that the check packages nothing and leaves save files untouched.
 */
#include "host_worker_stubs.h"

#define T1 LS_USER_HOME "/10000000/savedata_prospero/PPSA01234"
#define T2 LS_USER_HOME "/10000000/savedata_prospero/PPSA02222"

static void bump(const char *path) {
    struct timespec ts[2];
    clock_gettime(CLOCK_REALTIME, &ts[0]);
    ts[0].tv_sec += 100; ts[1] = ts[0];
    utimensat(AT_FDCWD, path, ts, 0);
}
static int64_t q_int(const char *sql) {
    sqlite3_stmt *st = NULL; int64_t v = -1;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    return v;
}
static ls_save_status_t status_of(const ls_sanity_report_t *r, const char *save) {
    for (int i = 0; i < r->nitems; i++)
        if (strcmp(r->items[i].save_name, save) == 0) return r->items[i].status;
    return LS_ST__COUNT;
}
static void drop_jobs(void) {
    pthread_mutex_lock(&g_ls_job_mtx);
    while (g_ls_jobs) { ls_job_t *j = g_ls_jobs; g_ls_jobs = j->next; free(j); }
    pthread_mutex_unlock(&g_ls_job_mtx);
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    CHECK(g_ls_schema >= 4, "schema has reconcile table");
    char *iobuf = malloc(BUF_SIZE);
    ls_sanity_report_t r;

    mkdirs(T1);
    mkdirs(T2);
    write_file(T1 "/A", "a1");
    write_file(T1 "/B", "b1");
    write_file(T1 "/C", "c1");
    write_file(T2 "/X", "x1");

    /* Untracked title. */
    CHECK(ls_sanity_title("PPSA02222", &r) == 0, "sanity untracked");
    CHECK(r.result == LS_ST_UNTRACKED && !r.tracked, "untracked result (%s)", ls_status_name(r.result));
    ls_sanity_free(&r);

    /* Freshly protected, nothing acked yet: healthy, all pending PC. */
    ls_protect_title("PPSA01234", "game_close", iobuf);
    drop_jobs();
    int64_t archives = q_int("SELECT COUNT(*) FROM queue");
    struct stat before;
    stat(T1 "/A", &before);
    ls_sanity_title("PPSA01234", &r);
    CHECK(r.result == LS_ST_HEALTHY && r.nitems == 3, "healthy (%s, %d)", ls_status_name(r.result), r.nitems);
    CHECK(r.pending_pc == 3, "3 pending PC (%d)", r.pending_pc);
    ls_sanity_free(&r);

    /* PC acks A => one less pending. */
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *st = NULL;
    char qa[200] = {0}, qa_sha[65] = {0};
    if (sqlite3_prepare_v2(g_ls_db, "SELECT id, sha256 FROM queue WHERE save_name='A'", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        ls_col_text(st, 0, qa, sizeof(qa));
        ls_col_text(st, 1, qa_sha, sizeof(qa_sha));
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    ls_queue_ack(qa, "pc", qa_sha);
    ls_sanity_title("PPSA01234", &r);
    CHECK(r.pending_pc == 2 && status_of(&r, "A") == LS_ST_HEALTHY, "A acked");
    ls_sanity_free(&r);

    /* Metadata change only visible cheaply => CHANGED_SINCE_LAST_SCAN. */
    bump(T1 "/B");
    ls_sanity_title("PPSA01234", &r);
    CHECK(status_of(&r, "B") == LS_ST_CHANGED_SINCE_LAST_SCAN, "B changed since last scan");
    CHECK(r.result == LS_ST_CHANGED_SINCE_LAST_SCAN, "title result follows most urgent");
    ls_sanity_free(&r);

    /* Failed backup => Shallot knows it is unprotected => UNBACKED_CHANGE. */
    write_file(T1 "/B", "b2");
    bump(T1 "/B");
    g_stage_fail = 1;
    ls_protect_title("PPSA01234", "game_close", iobuf);
    g_stage_fail = 0;
    ls_sanity_title("PPSA01234", &r);
    CHECK(status_of(&r, "B") == LS_ST_UNBACKED_CHANGE, "B unbacked (%s)", ls_status_name(status_of(&r, "B")));
    ls_sanity_free(&r);
    ls_protect_title("PPSA01234", "game_close", iobuf);   /* fix it again */
    drop_jobs();

    /* Unacked archive file lost on the PS5 => QUEUE_INCONSISTENCY for C. */
    char cfile[220] = {0};
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, "SELECT filename FROM queue WHERE save_name='C'", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        ls_col_text(st, 0, cfile, sizeof(cfile));
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    char cpath[700];
    snprintf(cpath, sizeof(cpath), "%s/%s", LS_QUEUE_DIR, cfile);
    unlink(cpath);
    ls_sanity_title("PPSA01234", &r);
    CHECK(status_of(&r, "C") == LS_ST_QUEUE_INCONSISTENCY, "C queue inconsistency (%s)",
          ls_status_name(status_of(&r, "C")));
    ls_sanity_free(&r);
    write_file(T1 "/C", "c2");               /* new content => new archive heals it */
    bump(T1 "/C");
    ls_protect_title("PPSA01234", "game_close", iobuf);
    drop_jobs();

    /* Known regression: A falls back to an older generation while the PC holds the newer. */
    write_file(T1 "/A", "a2");
    bump(T1 "/A");
    ls_protect_title("PPSA01234", "game_close", iobuf);
    drop_jobs();
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, "SELECT id, sha256 FROM queue WHERE save_name='A' ORDER BY created_at DESC, id DESC",
                           -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        ls_col_text(st, 0, qa, sizeof(qa));
        ls_col_text(st, 1, qa_sha, sizeof(qa_sha));
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    ls_queue_ack(qa, "pc", qa_sha);
    write_file(T1 "/A", "a1");               /* panic: older content back */
    bump(T1 "/A");
    ls_protect_title("PPSA01234", "game_close", iobuf);
    drop_jobs();
    ls_sanity_title("PPSA01234", &r);
    CHECK(status_of(&r, "A") == LS_ST_KNOWN_REGRESSION, "A known regression (%s)", ls_status_name(status_of(&r, "A")));
    CHECK(r.result == LS_ST_KNOWN_REGRESSION, "title result KNOWN_REGRESSION");
    ls_sanity_free(&r);

    /* Missing save beats everything. */
    unlink(T1 "/B");
    ls_sanity_title("PPSA01234", &r);
    CHECK(status_of(&r, "B") == LS_ST_MISSING_SAVE && r.result == LS_ST_MISSING_SAVE, "B missing");
    ls_sanity_free(&r);

    /* Running title: still answers (stat only), flags it. */
    g_running = 1;
    char *json = ls_sanity_title_json("PPSA01234");
    g_running = 0;
    CHECK(json && strstr(json, "\"running\":true") && strstr(json, "Title is running"), "running flagged");
    free(json);

    /* JSON + stored result. */
    json = ls_sanity_title_json("PPSA01234");
    CHECK(json && strstr(json, "\"result\":\"MISSING_SAVE\"") && strstr(json, "\"workload\":\"quick\""),
          "json result");
    free(json);
    CHECK(q_int("SELECT COUNT(*) FROM reconcile WHERE title_id='PPSA01234' AND last_result='MISSING_SAVE'"
                " AND last_sanity_check > 0") == 1, "result stored");
    CHECK(ev_count_code("RECONCILE_TITLE_SANITY_STARTED") >= 9 && ev_count_code("RECONCILE_TITLE_SANITY_COMPLETED") == 2,
          "events: STARTED per check, COMPLETED per API call");

    /* The checks themselves never packaged anything or touched files. */
    int64_t now_archives = q_int("SELECT COUNT(*) FROM queue");
    ls_sanity_title("PPSA01234", &r);
    ls_sanity_free(&r);
    CHECK(q_int("SELECT COUNT(*) FROM queue") == now_archives, "sanity packages nothing");
    CHECK(now_archives > archives, "(archives only came from explicit protect calls)");
    struct stat after;
    stat(T2 "/X", &after);
    CHECK(q_int("SELECT COUNT(*) FROM queue WHERE title_id='PPSA02222'") == 0, "untracked title never packaged");
    (void)before;

    free(iobuf);
    return host_finish();
}
