/*
 * Host tests for startup reconciliation (stage F): classification after a
 * restart, protect jobs only for tracked titles, missing saves flagged but
 * nothing deleted, running title skipped.
 */
#include "host_worker_stubs.h"

#define TRACKED LS_USER_HOME "/10000000/savedata_prospero/PPSA01234"
#define OTHER   LS_USER_HOME "/10000000/savedata_prospero/PPSA09999"

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
/* Pops all pending jobs; returns how many were for kind/title. */
static int take_jobs(int kind, const char *title) {
    int n = 0;
    pthread_mutex_lock(&g_ls_job_mtx);
    while (g_ls_jobs) {
        ls_job_t *j = g_ls_jobs;
        g_ls_jobs = j->next;
        if (j->kind == kind && strcmp(j->title_id, title) == 0) n++;
        free(j);
    }
    pthread_mutex_unlock(&g_ls_job_mtx);
    return n;
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    /* Scenarios 1-6 use the conservative setting; scenario 7 the default. */
    g_config_json = "{\"startup_protect_untracked\": false}";
    char *iobuf = malloc(BUF_SIZE);
    ls_reconcile_counts_t c;

    mkdirs(TRACKED);
    mkdirs(OTHER);
    write_file(TRACKED "/SAVE0000", "t0");
    write_file(TRACKED "/SAVE0001", "t1");
    write_file(OTHER "/SLOT", "o0");
    ls_protect_title("PPSA01234", "game_close", iobuf);     /* PPSA01234 becomes tracked */
    int64_t archives = q_int("SELECT COUNT(*) FROM queue");
    CHECK(archives == 2, "baseline archives");

    /* 1. Clean restart: nothing to do; the untracked title is only observed. */
    ls_startup_reconcile(&c);
    CHECK(c.unchanged == 2 && c.changed == 0 && c.new_saves == 0 && c.missing == 0, "clean: %d/%d/%d/%d",
          c.unchanged, c.changed, c.new_saves, c.missing);
    CHECK(c.untracked == 1 && c.titles_queued == 0, "untracked observed, nothing queued");
    CHECK(q_int("SELECT COUNT(*) FROM saves WHERE title_id='PPSA09999' AND state='untracked'") == 1,
          "untracked baseline row");
    CHECK(take_jobs(LS_JOB_TITLE_STARTUP, "PPSA01234") == 0, "no jobs");

    /* 2. "After a crash": one save changed, one new, one gone. */
    write_file(TRACKED "/SAVE0000", "t0-changed");
    bump(TRACKED "/SAVE0000");
    write_file(TRACKED "/SAVE0002", "t2-new");
    unlink(TRACKED "/SAVE0001");
    write_file(OTHER "/SLOT", "o0-changed");               /* untracked: still not packaged */
    bump(OTHER "/SLOT");
    ls_startup_reconcile(&c);
    CHECK(c.changed == 1 && c.new_saves == 1 && c.missing == 1, "crash: changed=%d new=%d missing=%d",
          c.changed, c.new_saves, c.missing);
    CHECK(c.titles_queued == 1, "one protect job");
    CHECK(take_jobs(LS_JOB_TITLE_STARTUP, "PPSA01234") == 1, "job is for the tracked title");
    CHECK(q_int("SELECT COUNT(*) FROM saves WHERE save_name='SAVE0001' AND state='missing'") == 1, "missing flagged");
    CHECK(ev_count_code("SAVE_MISSING") == 1, "SAVE_MISSING logged");
    CHECK(q_int("SELECT COUNT(*) FROM queue") == archives, "reconcile itself packages nothing");
    CHECK(q_int("SELECT COUNT(*) FROM queue WHERE title_id='PPSA01234' AND save_name='SAVE0001'"
                " AND state='ready'") == 1, "archive of the missing save is kept");

    /* 3. The queued job protects the changed and new saves (stability ~4 s). */
    ls_job_t job = { LS_JOB_TITLE_STARTUP, "PPSA01234", 0, 0, 0, "", NULL };
    ls_run_job(&job, iobuf);
    CHECK(q_int("SELECT COUNT(*) FROM queue WHERE reason='startup_reconcile'") == 2, "two startup archives");
    take_jobs(0, "");

    /* 4. Second restart right after: everything tracked is unchanged again;
          missing stays missing and is not reported twice. */
    ls_startup_reconcile(&c);
    CHECK(c.changed == 0 && c.new_saves == 0 && c.titles_queued == 0 && c.missing == 0,
          "idempotent: changed=%d new=%d queued=%d missing=%d", c.changed, c.new_saves, c.titles_queued, c.missing);
    CHECK(ev_count_code("SAVE_MISSING") == 1, "missing not re-reported");

    /* 5. Running title is skipped entirely (not read, not marked missing). */
    write_file(TRACKED "/SAVE0000", "t0-while-running");
    bump(TRACKED "/SAVE0000");
    unlink(TRACKED "/SAVE0002");
    g_running = 1;
    ls_startup_reconcile(&c);
    g_running = 0;
    CHECK(c.skipped_running == 1 && c.titles_queued == 0 && c.missing == 0, "running skipped (%d)", c.skipped_running);
    CHECK(q_int("SELECT COUNT(*) FROM saves WHERE save_name='SAVE0002' AND state='missing'") == 0,
          "running title's saves not marked missing");
    take_jobs(0, "");

    /* 6. A missing save that reappears is picked up again. */
    write_file(TRACKED "/SAVE0001", "t1-restored-by-user");
    ls_startup_reconcile(&c);
    CHECK(c.changed >= 1 && c.titles_queued == 1, "reappeared save queued");
    take_jobs(0, "");

    /* 7. Default (owner preference): untracked titles get a one-time protect job. */
    g_config_json = NULL;
    ls_startup_reconcile(&c);
    CHECK(c.untracked == 0 && c.titles_queued >= 1, "untracked title queued by default (%d, %d)",
          c.untracked, c.titles_queued);
    CHECK(take_jobs(LS_JOB_TITLE_STARTUP, "PPSA09999") == 1, "job for the never-protected title");

    free(iobuf);
    return host_finish();
}
