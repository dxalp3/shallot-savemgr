/*
 * Host tests for src/lansync_worker.inc.c: per-save protection decisions,
 * no-duplicate rules, change-during-packaging guard, running-title deferral,
 * and one end-to-end job with the real stability detector.
 */
#include "host_worker_stubs.h"

#define DIR5 LS_USER_HOME "/10000000/savedata_prospero/PPSA01234"

static int64_t queue_rows(void) {
    sqlite3_stmt *st = NULL; int64_t n = -1;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, "SELECT COUNT(*) FROM queue WHERE state='ready'", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    return n;
}
static void save_state(const char *save, char *out, size_t n) {
    ls_save_ref_t s; memset(&s, 0, sizeof(s));
    snprintf(s.user_id, sizeof(s.user_id), "10000000");
    snprintf(s.title_id, sizeof(s.title_id), "PPSA01234");
    snprintf(s.save_name, sizeof(s.save_name), "%s", save);
    ls_save_row_t r; ls_saves_get(&s, &r);
    snprintf(out, n, "%s", r.found ? r.state : "(none)");
}
static void bump_mtime(const char *path) {
    struct timespec ts[2];
    clock_gettime(CLOCK_REALTIME, &ts[0]);
    ts[0].tv_sec += 100; ts[1] = ts[0];
    utimensat(AT_FDCWD, path, ts, 0);
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    char *iobuf = malloc(BUF_SIZE);
    char st[32];

    mkdirs(DIR5);
    write_file(DIR5 "/SAVE0000", "slot-0-v1");
    write_file(DIR5 "/SAVE0001", "slot-1-v1");

    /* 1. First protection packages every save. */
    CHECK(ls_protect_title("PPSA01234", "game_close", iobuf) == 0, "first run leaves nothing");
    CHECK(queue_rows() == 2, "two archives queued (%lld)", (long long)queue_rows());
    save_state("SAVE0000", st, sizeof(st));
    CHECK(strcmp(st, "protected") == 0, "state protected: %s", st);

    /* 2. No change => no new archive. */
    ls_protect_title("PPSA01234", "game_close", iobuf);
    CHECK(queue_rows() == 2, "no duplicate on no change");

    /* 3. mtime-only change, identical bytes => no new archive. */
    bump_mtime(DIR5 "/SAVE0000");
    ls_protect_title("PPSA01234", "game_close", iobuf);
    CHECK(queue_rows() == 2, "metadata-only change makes no archive");
    CHECK(ev_count_code("SAVE_UNCHANGED") >= 1, "SAVE_UNCHANGED logged");

    /* 4. Real content change => exactly one new archive. */
    write_file(DIR5 "/SAVE0001", "slot-1-v2");
    bump_mtime(DIR5 "/SAVE0001");
    ls_protect_title("PPSA01234", "game_close", iobuf);
    CHECK(queue_rows() == 3, "changed save archived once (%lld)", (long long)queue_rows());

    /* 5. Packaging failure => stays 'changed', protected only after a later success. */
    write_file(DIR5 "/SAVE0000", "slot-0-v2");
    bump_mtime(DIR5 "/SAVE0000");
    g_stage_fail = 1;
    CHECK(ls_protect_title("PPSA01234", "game_close", iobuf) == 1, "one save left unprotected");
    g_stage_fail = 0;
    save_state("SAVE0000", st, sizeof(st));
    CHECK(strcmp(st, "changed") == 0, "failed save is 'changed': %s", st);
    CHECK(queue_rows() == 3, "no archive on failure");
    ls_protect_title("PPSA01234", "game_close", iobuf);
    save_state("SAVE0000", st, sizeof(st));
    CHECK(strcmp(st, "protected") == 0 && queue_rows() == 4, "retry protects it");

    /* 6. Save modified while zipping => archive discarded, not protected. */
    write_file(DIR5 "/SAVE0001", "slot-1-v3");
    bump_mtime(DIR5 "/SAVE0001");
    g_mutate_during_stage = DIR5 "/SAVE0001";
    CHECK(ls_protect_title("PPSA01234", "game_close", iobuf) == 1, "mutated save left");
    save_state("SAVE0001", st, sizeof(st));
    CHECK(strcmp(st, "changed") == 0 && queue_rows() == 4, "mutated archive discarded (%s)", st);
    DIR *d = opendir(BACKUP_TMP_DIR); int leftovers = 0; struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') leftovers++;
    closedir(d);
    CHECK(leftovers == 0, "no stage leftovers (%d)", leftovers);

    /* 7. Title running => nothing is read or packaged. */
    g_running = 1;
    CHECK(ls_protect_title("PPSA01234", "game_close", iobuf) == 2, "running title deferred");
    CHECK(queue_rows() == 4, "nothing archived while running");
    ls_job_t job = { LS_JOB_TITLE_CLOSED, "PPSA01234", 0, 0, 0, "", NULL };
    ls_run_job(&job, iobuf);
    CHECK(queue_rows() == 4 && ev_count_code("BACKUP_DEFERRED") >= 2, "job deferred while running");
    g_running = 0;

    /* 8. End-to-end job with the real stability detector (takes ~4 s). */
    ls_run_job(&job, iobuf);
    save_state("SAVE0001", st, sizeof(st));
    CHECK(strcmp(st, "protected") == 0 && queue_rows() == 5, "job protects after stability (%s, %lld)",
          st, (long long)queue_rows());
    CHECK(ev_count_code("SAVE_STABLE") >= 1, "SAVE_STABLE logged");

    /* 8b. The close job scheduled one delayed follow-up re-check. */
    {
        int followups = 0;
        int64_t now = ls_io_now_ms(NULL), nb = 0;
        pthread_mutex_lock(&g_ls_job_mtx);
        while (g_ls_jobs) {
            ls_job_t *j = g_ls_jobs;
            g_ls_jobs = j->next;
            if (j->kind == LS_JOB_TITLE_FOLLOWUP && strcmp(j->title_id, "PPSA01234") == 0) {
                followups++;
                nb = j->not_before_ms;
            }
            free(j);
        }
        pthread_mutex_unlock(&g_ls_job_mtx);
        CHECK(followups == 1 && nb - now > 170000, "one follow-up ~180 s later (%d)", followups);
        ls_job_t fu = { LS_JOB_TITLE_FOLLOWUP, "PPSA01234", 0, 0, 0, "", NULL };
        int64_t before = queue_rows();
        ls_run_job(&fu, iobuf);
        CHECK(queue_rows() == before, "follow-up with no change packages nothing");
        pthread_mutex_lock(&g_ls_job_mtx);
        CHECK(g_ls_jobs == NULL, "follow-up does not schedule another follow-up");
        pthread_mutex_unlock(&g_ls_job_mtx);
    }

    /* 9. Archive content holds the save bytes and the sha in the queue row matches the file. */
    ls_save_ref_t s; memset(&s, 0, sizeof(s));
    snprintf(s.user_id, sizeof(s.user_id), "10000000");
    snprintf(s.title_id, sizeof(s.title_id), "PPSA01234");
    snprintf(s.save_name, sizeof(s.save_name), "SAVE0001");
    ls_save_row_t row; ls_saves_get(&s, &row);
    ls_queue_row_t q;
    CHECK(ls_queue_get(row.last_queue_id, &q), "last queue id resolves");
    char qp[700], hex[65]; snprintf(qp, sizeof(qp), "%s/%s", LS_QUEUE_DIR, q.filename);
    ls_sha256_file(qp, hex, NULL, iobuf);
    CHECK(strcmp(hex, q.sha256) == 0, "queue sha matches archive");

    /* 10. Generations: one per archived content, chained by parent. */
    CHECK(q.gen_id > 0 && q.gen_id == row.current_gen_id, "queue row and save point at the same generation");
    int64_t g_last = row.current_gen_id;
    CHECK(ls_gen_latest(&s) == g_last, "latest generation is current");
    char gsql[200];
    snprintf(gsql, sizeof(gsql), "SELECT parent_gen FROM generations WHERE gen_id = %lld", (long long)g_last);
    sqlite3_stmt *gst = NULL; int64_t parent = -1;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, gsql, -1, &gst, NULL) == SQLITE_OK && sqlite3_step(gst) == SQLITE_ROW)
        parent = sqlite3_column_int64(gst, 0);
    sqlite3_finalize(gst);
    pthread_mutex_unlock(&g_ls_mtx);
    CHECK(parent > 0 && parent < g_last, "parent link to previous generation (%lld)", (long long)parent);

    /* 11. Panic regression: content falls back to an older generation that is
     *     already archived. No new archive; reported, nothing restored. */
    write_file(DIR5 "/SAVE0001", "slot-1-v2");            /* = content of an earlier generation */
    bump_mtime(DIR5 "/SAVE0001");
    int64_t rows_before = queue_rows();
    ls_protect_title("PPSA01234", "game_close", iobuf);
    CHECK(queue_rows() == rows_before, "older content makes no new archive");
    CHECK(ev_count_code("SAVE_MATCHES_OLDER_GENERATION") == 1, "older generation reported (not PC-acked yet)");
    ls_saves_get(&s, &row);
    CHECK(row.current_gen_id > 0 && row.current_gen_id < g_last, "current generation moved back");

    /* Now the PC acknowledges the newest generation; the same fallback is a known regression. */
    ls_queue_get(q.id, &q);
    CHECK(ls_queue_ack(q.id, "pc", q.sha256) == 0, "ack newest");
    write_file(DIR5 "/SAVE0001", "slot-1-v3+late-write");  /* back to newest content */
    bump_mtime(DIR5 "/SAVE0001");
    ls_protect_title("PPSA01234", "game_close", iobuf);
    ls_saves_get(&s, &row);
    CHECK(row.current_gen_id == g_last && queue_rows() == rows_before, "returning to newest: no archive, gen restored");
    write_file(DIR5 "/SAVE0001", "slot-1-v2");
    bump_mtime(DIR5 "/SAVE0001");
    ls_protect_title("PPSA01234", "game_close", iobuf);
    CHECK(ev_count_code("KNOWN_REGRESSION_DETECTED") == 1, "known regression reported once PC holds newer gen");
    CHECK(queue_rows() == rows_before, "still no new archive");

    free(iobuf);
    return host_finish();
}
