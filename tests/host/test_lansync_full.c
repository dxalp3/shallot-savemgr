/*
 * Host tests for Full Reconcile Game (stage H): hash-based classification,
 * documented bookkeeping repairs, archive integrity, protect variant,
 * running title, cancellation and the single job slot.
 */
#include "host_worker_stubs.h"

#define T1 LS_USER_HOME "/10000000/savedata_prospero/PPSA01234"

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
static void q_text(const char *sql, char *out, size_t n) {
    sqlite3_stmt *st = NULL; out[0] = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        ls_col_text(st, 0, out, n);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
}
static int drop_jobs(int kind) {
    int n = 0;
    pthread_mutex_lock(&g_ls_job_mtx);
    while (g_ls_jobs) { ls_job_t *j = g_ls_jobs; g_ls_jobs = j->next; if (j->kind == kind) n++; free(j); }
    pthread_mutex_unlock(&g_ls_job_mtx);
    return n;
}
static ls_save_status_t st_of(const ls_full_report_t *r, const char *save) {
    for (int i = 0; i < r->nitems; i++)
        if (strcmp(r->items[i].save_name, save) == 0) return r->items[i].status;
    return LS_ST__COUNT;
}
/* Runs the full reconcile synchronously inside the job slot. */
static void full(ls_full_report_t *r, char *iobuf) {
    ls_rjob_claim("title_full", "PPSA01234", 0);
    ls_full_title("PPSA01234", r, iobuf);
    ls_rjob_finish("completed", "");
}
static void ack_latest(const char *save) {
    char sql[300], id[200], sha[65];
    snprintf(sql, sizeof(sql), "SELECT id FROM queue WHERE save_name='%s' AND state='ready' ORDER BY rowid DESC", save);
    q_text(sql, id, sizeof(id));
    snprintf(sql, sizeof(sql), "SELECT sha256 FROM queue WHERE id='%s'", id);
    q_text(sql, sha, sizeof(sha));
    ls_queue_ack(id, "pc", sha);
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    char *iobuf = malloc(BUF_SIZE);
    ls_full_report_t r;
    ls_sanity_report_t sr;

    mkdirs(T1);
    write_file(T1 "/A", "a1");
    write_file(T1 "/B", "b1");
    write_file(T1 "/C", "c1");
    ls_protect_title("PPSA01234", "game_close", iobuf);
    drop_jobs(0);

    /* 1. Healthy, archives verified. */
    full(&r, iobuf);
    CHECK(r.result == LS_ST_HEALTHY && r.nitems == 3 && r.archives_checked == 3 && r.archives_corrupt == 0,
          "healthy (%s, %d, %d)", ls_status_name(r.result), r.nitems, r.archives_checked);
    CHECK(r.repairs == 0 && r.pending_pc == 3, "no repairs, 3 pending PC");
    free(r.items);

    /* 2. Metadata-only change: sanity is unsure, full proves it and repairs bookkeeping. */
    bump(T1 "/A");
    ls_sanity_title("PPSA01234", &sr);
    CHECK(sr.result == LS_ST_CHANGED_SINCE_LAST_SCAN, "sanity unsure");
    ls_sanity_free(&sr);
    int64_t archives = q_int("SELECT COUNT(*) FROM queue");
    full(&r, iobuf);
    CHECK(st_of(&r, "A") == LS_ST_HEALTHY && r.repairs == 1, "full: healthy + 1 repair (%d)", r.repairs);
    free(r.items);
    ls_sanity_title("PPSA01234", &sr);
    CHECK(sr.result == LS_ST_HEALTHY, "sanity healthy after repair");
    ls_sanity_free(&sr);
    CHECK(q_int("SELECT COUNT(*) FROM queue") == archives, "full reconcile packaged nothing");

    /* 3. Real unbacked change; plain full only reports, protect variant queues a job. */
    write_file(T1 "/B", "b2");
    bump(T1 "/B");
    full(&r, iobuf);
    CHECK(st_of(&r, "B") == LS_ST_UNBACKED_CHANGE && r.result == LS_ST_UNBACKED_CHANGE, "B unbacked");
    free(r.items);
    CHECK(drop_jobs(LS_JOB_TITLE_MANUAL) == 0, "plain full queues nothing");
    g_ls_worker_started = 1;              /* jobs are only queued when a worker exists */
    ls_rjob_claim("title_full", "PPSA01234", 1);
    ls_full_title_run("PPSA01234", 1, iobuf);
    g_ls_worker_started = 0;
    CHECK(drop_jobs(LS_JOB_TITLE_MANUAL) == 1, "protect variant queued one job");
    CHECK(q_int("SELECT COUNT(*) FROM reconcile WHERE title_id='PPSA01234' AND last_full_reconcile > 0"
                " AND last_result='UNBACKED_CHANGE'") == 1, "full result stored");
    ls_protect_title("PPSA01234", "manual_changed", iobuf);
    drop_jobs(0);

    /* 4. Regression with the newer version on the PC. */
    ack_latest("B");
    write_file(T1 "/B", "b1");
    bump(T1 "/B");
    full(&r, iobuf);
    CHECK(st_of(&r, "B") == LS_ST_KNOWN_REGRESSION && r.result == LS_ST_KNOWN_REGRESSION,
          "B regression (%s)", ls_status_name(st_of(&r, "B")));
    free(r.items);
    write_file(T1 "/B", "b2");      /* back to newest */
    bump(T1 "/B");

    /* 5. Corrupt archive: detected, reported, kept. */
    char cfile[220], cpath[700];
    q_text("SELECT filename FROM queue WHERE save_name='C' AND state='ready'", cfile, sizeof(cfile));
    snprintf(cpath, sizeof(cpath), "%s/%s", LS_QUEUE_DIR, cfile);
    FILE *f = fopen(cpath, "r+b"); fputc('X', f); fclose(f);
    full(&r, iobuf);
    CHECK(r.archives_corrupt == 1 && r.result == LS_ST_QUEUE_INCONSISTENCY, "corrupt archive found (%d, %s)",
          r.archives_corrupt, ls_status_name(r.result));
    free(r.items);
    CHECK(file_exists(cpath), "corrupt archive kept");
    CHECK(ev_count_code("QUEUE_ARCHIVE_CORRUPT") == 1, "corrupt event");

    /* 6. Current generation lost its only copy (not on PC, file gone). */
    unlink(cpath);
    full(&r, iobuf);
    CHECK(st_of(&r, "C") == LS_ST_QUEUE_INCONSISTENCY, "C has no surviving archive (%s)",
          ls_status_name(st_of(&r, "C")));
    CHECK(r.archives_missing == 1, "missing archive counted");
    free(r.items);
    char cstate[16];
    q_text("SELECT state FROM queue WHERE save_name='C' ORDER BY rowid LIMIT 1", cstate, sizeof(cstate));
    CHECK(strcmp(cstate, "missing") == 0, "queue row repaired to 'missing'");

    /* 7. Running title: nothing read, nothing stored. */
    int64_t stored_at = q_int("SELECT last_full_reconcile FROM reconcile WHERE title_id='PPSA01234'");
    g_running = 1;
    ls_rjob_claim("title_full", "PPSA01234", 0);
    ls_full_title_run("PPSA01234", 0, iobuf);
    g_running = 0;
    CHECK(q_int("SELECT last_full_reconcile FROM reconcile WHERE title_id='PPSA01234'") == stored_at,
          "running: nothing stored");
    CHECK(strcmp(g_ls_rjob.last_status, "completed") == 0 && strstr(g_ls_rjob.last_summary, "running"),
          "running reported in summary");

    /* 8. Cancellation before the first save: partial, not stored. */
    ls_rjob_claim("title_full", "PPSA01234", 0);
    g_ls_rjob.cancel = 1;
    ls_full_title_run("PPSA01234", 0, iobuf);
    CHECK(strcmp(g_ls_rjob.last_status, "cancelled") == 0, "cancelled status");
    CHECK(q_int("SELECT last_full_reconcile FROM reconcile WHERE title_id='PPSA01234'") == stored_at,
          "cancelled: nothing stored");
    CHECK(ev_count_code("RECONCILE_CANCELLED") == 1, "cancel event");

    /* 9. Single job slot. */
    CHECK(ls_rjob_claim("title_full", "X", 0) == 0, "claim");
    CHECK(ls_full_title_start("PPSA01234", 0) == -1, "second job refused while busy");
    ls_rjob_finish("completed", "");

    /* 10. Background start really runs and finishes. */
    CHECK(ls_full_title_start("PPSA01234", 0) == 0, "background start");
    for (int i = 0; i < 100 && g_ls_rjob.active; i++) usleep(50000);
    CHECK(!g_ls_rjob.active && strcmp(g_ls_rjob.last_status, "completed") == 0, "background job finished");

    free(iobuf);
    return host_finish();
}
