/*
 * Host tests for the idle sweep (stage K): timer behaviour, sweep
 * classification (only changed/new saves get protect jobs), idle-specific
 * events and bookkeeping, no repeated SAVE_NOT_SCANNED logging.
 */
#include "host_worker_stubs.h"

#define T1  LS_USER_HOME "/10000000/savedata_prospero/PPSA01234"
#define DBD LS_SYSTEM_DATA "/savedata_prospero/10000000/db/user"

static int jobs_of(int kind, const char *title) {
    int n = 0;
    pthread_mutex_lock(&g_ls_job_mtx);
    for (ls_job_t *j = g_ls_jobs; j; j = j->next)
        if (j->kind == kind && (!title || strcmp(j->title_id, title) == 0)) n++;
    pthread_mutex_unlock(&g_ls_job_mtx);
    return n;
}
static void drop_jobs(void) {
    pthread_mutex_lock(&g_ls_job_mtx);
    while (g_ls_jobs) { ls_job_t *j = g_ls_jobs; g_ls_jobs = j->next; free(j); }
    pthread_mutex_unlock(&g_ls_job_mtx);
}
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

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    char *iobuf = malloc(BUF_SIZE);

    /* 1. Timer (defaults: idle_delay_min = 10 => 600 s, repeat 3600 s). */
    g_ls_worker_started = 1;
    CHECK(ls_idle_tick(0, 1000) == 0, "first tick only starts the clock");
    CHECK(ls_idle_tick(0, 1599) == 0, "not idle long enough");
    CHECK(ls_idle_tick(0, 1600) == 1 && jobs_of(LS_JOB_IDLE_SWEEP, "") == 1, "sweep after 10 min idle");
    drop_jobs();
    CHECK(ls_idle_tick(0, 1700) == 0, "no second sweep right away");
    CHECK(ls_idle_tick(0, 1600 + 3599) == 0, "repeat interval not reached");
    CHECK(ls_idle_tick(0, 1600 + 3600) == 1, "repeat sweep after 6x delay");
    drop_jobs();
    CHECK(ls_idle_tick(1, 6000) == 0, "running title = activity");
    CHECK(ls_idle_tick(0, 6599) == 0 && ls_idle_tick(0, 6600) == 1, "new idle period => sweep after delay");
    drop_jobs();
    g_ls_worker_started = 0;
    CHECK(ls_idle_tick(0, 99999) == 0, "no worker => no sweep");

    /* 2. Sweep classification. */
    mkdirs(T1);
    write_file(T1 "/A", "a1");
    write_file(T1 "/B", "b1");
    ls_protect_title("PPSA01234", "game_close", iobuf);
    drop_jobs();
    ls_reconcile_counts_t c;
    ls_scan_reconcile(1, &c);
    CHECK(c.unchanged == 2 && c.titles_queued == 0 && jobs_of(LS_JOB_TITLE_IDLE, NULL) == 0, "nothing changed => no job");
    write_file(T1 "/A", "a2");
    bump(T1 "/A");
    ls_scan_reconcile(1, &c);
    CHECK(c.changed == 1 && c.titles_queued == 1 && jobs_of(LS_JOB_TITLE_IDLE, "PPSA01234") == 1, "changed => idle job");
    CHECK(ev_count_code("IDLE_SCAN_COMPLETED") == 2 && ev_count_code("IDLE_SCAN_CHANGED_COUNT") == 2, "idle events");
    CHECK(ev_count_code("STARTUP_RECONCILIATION_COMPLETED") == 0, "no startup events from idle sweep");
    CHECK(q_int("SELECT value FROM meta WHERE key='last_idle_scan'") > 0, "last_idle_scan recorded");
    drop_jobs();

    /* 3. The idle protect job packages with reason idle_sweep. */
    ls_job_t job = { LS_JOB_TITLE_IDLE, "PPSA01234", 0, 0, 0, "", NULL };
    ls_run_job(&job, iobuf);
    CHECK(q_int("SELECT COUNT(*) FROM queue WHERE reason='idle_sweep' AND save_name='A'") == 1, "archived as idle_sweep");
    CHECK(jobs_of(LS_JOB_TITLE_FOLLOWUP, NULL) == 0, "idle jobs schedule no follow-up");

    /* 4. Unscanned saves are counted but not re-logged every sweep. */
    mkdirs(DBD);
    mkdirs(T1 "/sdimg_DIRSAVE");
    sqlite3 *db = NULL;
    sqlite3_open(DBD "/savedata.db", &db);
    sqlite3_exec(db, "CREATE TABLE savedata(title_id TEXT, dir_name TEXT, mtime TEXT, main_title TEXT);"
                     "INSERT INTO savedata VALUES('PPSA01234','DIRSAVE','t','x');", NULL, NULL, NULL);
    sqlite3_close(db);
    ls_scan_reconcile(1, &c);
    CHECK(c.not_scanned == 1 && ev_count_code("SAVE_NOT_SCANNED") == 0, "idle: counted, not logged");
    ls_scan_reconcile(0, &c);
    CHECK(ev_count_code("SAVE_NOT_SCANNED") == 1, "startup: logged");

    /* 5. Job dispatch: LS_JOB_IDLE_SWEEP runs the sweep. */
    drop_jobs();
    int64_t before = ev_count_code("IDLE_SCAN_COMPLETED");
    ls_job_t sweep = { LS_JOB_IDLE_SWEEP, "", 0, 0, 0, "", NULL };
    ls_run_job(&sweep, iobuf);
    CHECK(ev_count_code("IDLE_SCAN_COMPLETED") == before + 1, "sweep job dispatched");
    drop_jobs();

    free(iobuf);
    return host_finish();
}
