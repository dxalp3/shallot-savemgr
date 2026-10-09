/*
 * Host tests for the catalogue jobs (stages I/J): title discovery from disk,
 * Shallot's records and the PS5 save DB; per-title results; running title
 * skipped; protect jobs only where content is unprotected; nothing packaged
 * by the audit itself; cancellation and the single job slot.
 */
#include "host_worker_stubs.h"

#define H(t) LS_USER_HOME "/10000000/savedata_prospero/" t

static int64_t q_int(const char *sql) {
    sqlite3_stmt *st = NULL; int64_t v = -1;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    return v;
}
static int jobs_for(int kind, const char *title) {
    int n = 0;
    pthread_mutex_lock(&g_ls_job_mtx);
    for (ls_job_t *j = g_ls_jobs; j; j = j->next) if (j->kind == kind && strcmp(j->title_id, title) == 0) n++;
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
static void run_cat(int full, int protect, char *iobuf) {
    ls_rjob_claim(full ? "catalogue_full" : "catalogue_quick", "", protect);
    ls_catalogue_run(full, protect, iobuf);
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    char *iobuf = malloc(BUF_SIZE);

    /* T1 healthy, T2 will change, T3 untracked, T4 will vanish, T5 will be running. */
    mkdirs(H("PPSA00001")); write_file(H("PPSA00001") "/S", "t1");
    mkdirs(H("PPSA00002")); write_file(H("PPSA00002") "/S", "t2");
    mkdirs(H("PPSA00003")); write_file(H("PPSA00003") "/S", "t3");
    mkdirs(H("PPSA00004")); write_file(H("PPSA00004") "/S", "t4");
    mkdirs(H("PPSA00005")); write_file(H("PPSA00005") "/S", "t5");
    ls_protect_title("PPSA00001", "game_close", iobuf);
    ls_protect_title("PPSA00002", "game_close", iobuf);
    ls_protect_title("PPSA00004", "game_close", iobuf);
    ls_protect_title("PPSA00005", "game_close", iobuf);
    drop_jobs();
    write_file(H("PPSA00002") "/S", "t2-changed");
    bump(H("PPSA00002") "/S");
    unlink(H("PPSA00004") "/S");
    rmdir(H("PPSA00004"));
    g_running = 1;
    g_running_title = "PPSA00005";
    int64_t archives = q_int("SELECT COUNT(*) FROM queue");

    /* Quick catalogue. */
    run_cat(0, 0, iobuf);
    CHECK(strcmp(g_ls_rjob.last_status, "completed") == 0, "quick completed");
    char *j = ls_meta_get_text("last_catalogue_quick");
    CHECK(j != NULL, "quick summary stored");
    CHECK(j && strstr(j, "\"titles\":5") && strstr(j, "\"checked\":4") && strstr(j, "\"skipped_running\":1"),
          "5 titles found (incl. vanished one), 4 checked, running skipped: %s", j ? j : "");
    CHECK(j && strstr(j, "\"title_id\":\"PPSA00002\"") && strstr(j, "\"title_id\":\"PPSA00004\""),
          "anomalous titles listed");
    CHECK(j && !strstr(j, "\"title_id\":\"PPSA00001\""), "healthy title not listed as anomaly");
    CHECK(j && strstr(j, "\"missing_saves\":1"), "missing save counted");
    free(j);
    CHECK(q_int("SELECT COUNT(*) FROM reconcile WHERE last_catalogue_scan > 0") == 4, "per-title results stored");
    CHECK(q_int("SELECT COUNT(*) FROM queue") == archives, "quick packaged nothing");
    CHECK(ev_count_code("RECONCILE_CATALOGUE_QUICK_COMPLETED") == 1, "completed event");

    /* Full catalogue + protect: only T2 (unprotected content) gets a job. */
    g_ls_worker_started = 1;
    run_cat(1, 1, iobuf);
    g_ls_worker_started = 0;
    CHECK(strcmp(g_ls_rjob.last_status, "completed") == 0, "full completed");
    CHECK(jobs_for(LS_JOB_TITLE_MANUAL, "PPSA00002") == 1, "protect job for changed title");
    CHECK(jobs_for(LS_JOB_TITLE_MANUAL, "PPSA00001") == 0 && jobs_for(LS_JOB_TITLE_MANUAL, "PPSA00005") == 0,
          "no job for healthy or running titles");
    j = ls_meta_get_text("last_catalogue_full");
    CHECK(j && strstr(j, "\"protect_jobs_queued\":1") && strstr(j, "\"unprotected_changes\":1"),
          "full summary: %s", j ? j : "");
    free(j);
    CHECK(q_int("SELECT COUNT(*) FROM queue") == archives, "full audit packaged nothing itself");
    drop_jobs();

    /* Plain full (no protect) queues nothing. */
    run_cat(1, 0, iobuf);
    CHECK(jobs_for(LS_JOB_TITLE_MANUAL, "PPSA00002") == 0, "no protect without protect=1");

    /* Cancellation before the first title. */
    ls_rjob_claim("catalogue_quick", "", 0);
    g_ls_rjob.cancel = 1;
    ls_catalogue_run(0, 0, iobuf);
    CHECK(strcmp(g_ls_rjob.last_status, "cancelled") == 0, "cancelled");

    /* Single slot shared with Full Reconcile. */
    ls_rjob_claim("title_full", "PPSA00001", 0);
    CHECK(ls_catalogue_start(0, 0) == -1, "catalogue refused while another job runs");
    ls_rjob_finish("completed", "");

    /* Background start finishes. */
    g_running = 0;
    CHECK(ls_catalogue_start(0, 0) == 0, "background quick start");
    for (int i = 0; i < 100 && g_ls_rjob.active; i++) usleep(50000);
    CHECK(!g_ls_rjob.active && strcmp(g_ls_rjob.last_status, "completed") == 0, "background finished");

    free(iobuf);
    return host_finish();
}
