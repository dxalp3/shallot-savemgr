/*
 * Host tests for the conservative restore (stage M): staging + hash checks,
 * scratch-copy validation (title / save dir / account), pre-restore
 * protection of the current save, atomic swap, verification, savedata.db
 * update or re-insert, bookkeeping, and every refusal path.
 */
#include "host_worker_stubs.h"

#define T    "PPSA01234"
#define U    "10000000"
#define D    LS_USER_HOME "/" U "/savedata_prospero/" T
#define SAVE "sdimg_SAVE0"
#define DBD  LS_SYSTEM_DATA "/savedata_prospero/" U "/db/user"

static void bump(const char *path) {
    struct timespec ts[2];
    clock_gettime(CLOCK_REALTIME, &ts[0]);
    ts[0].tv_sec += 100 + (rand() % 100); ts[1] = ts[0];
    utimensat(AT_FDCWD, path, ts, 0);
}
static void q_text(const char *sql, char *out, size_t n) {
    sqlite3_stmt *st = NULL; out[0] = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        ls_col_text(st, 0, out, n);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
}
static int64_t q_int(const char *sql) {
    char b[64]; q_text(sql, b, sizeof(b)); return atoll(b);
}
static void drop_jobs(void) {
    pthread_mutex_lock(&g_ls_job_mtx);
    while (g_ls_jobs) { ls_job_t *j = g_ls_jobs; g_ls_jobs = j->next; free(j); }
    pthread_mutex_unlock(&g_ls_job_mtx);
}
static void read_file(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "rb"); out[0] = 0;
    if (!f) return;
    size_t k = fread(out, 1, n - 1, f); out[k] = 0; fclose(f);
}
static void sysdb_mtime(char *out, size_t n) {
    sqlite3 *db = NULL; sqlite3_stmt *st = NULL; out[0] = 0;
    sqlite3_open(DBD "/savedata.db", &db);
    if (sqlite3_prepare_v2(db, "SELECT mtime FROM savedata WHERE title_id='" T "' AND dir_name='SAVE0'", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) snprintf(out, n, "%s", (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st); sqlite3_close(db);
}
/* Latest ready queue id for the save + its generation's container hash. */
static void latest_archive(char *qid, size_t qn, char *csha, int64_t *gen) {
    q_text("SELECT id FROM queue WHERE save_name='" SAVE "' AND state='ready' ORDER BY rowid DESC", qid, qn);
    char sql[400];
    snprintf(sql, sizeof(sql), "SELECT g.sha256 FROM generations g JOIN queue q ON q.gen_id=g.gen_id WHERE q.id='%s'", qid);
    q_text(sql, csha, 65);
    snprintf(sql, sizeof(sql), "SELECT gen_id FROM queue WHERE id='%s'", qid);
    *gen = q_int(sql);
}
/* Request a restore of queue archive qid and run it like the worker would. */
static int restore_queue(const char *qid, const char *csha, int64_t gen, char *iobuf, char *err, size_t en) {
    ls_queue_row_t q;
    if (!ls_queue_get(qid, &q)) { snprintf(err, en, "no queue row"); return -9; }
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", LS_QUEUE_DIR, q.filename);
    int rc = ls_restore_request("queue", U, T, SAVE, path, csha, gen, err, en);
    if (rc == 0) { drop_jobs(); ls_restore_run(iobuf); }
    return rc;
}
static int state_is(const char *st) {
    pthread_mutex_lock(&g_ls_restore_mtx);
    int r = strcmp(g_ls_restore.state, st) == 0;
    pthread_mutex_unlock(&g_ls_restore_mtx);
    return r;
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    g_ls_worker_started = 1;
    char *iobuf = malloc(BUF_SIZE);
    char err[300], buf[256], q1[200], q2[200], c1[65], c2[65], m0[64], m1[64];
    int64_t g1, g2;

    /* System save DB with the save's row, like the PS5 has it. */
    mkdirs(DBD);
    sqlite3 *db = NULL;
    sqlite3_open(DBD "/savedata.db", &db);
    sqlite3_exec(db, "CREATE TABLE savedata(id INTEGER PRIMARY KEY AUTOINCREMENT, title_id, dir_name, main_title, sub_title,"
                     " detail, tmp_dir_name, is_broken, user_param, blocks, free_blocks, size_kib, mtime, fake_broken,"
                     " account_id, user_id, faked_owner, cloud_icon_url, cloud_revision, game_title_id, system_blocks);"
                     "INSERT INTO savedata(title_id, dir_name, main_title, mtime, game_title_id)"
                     " VALUES('" T "','SAVE0','Test','2000-01-01T00:00:00.00Z','" T "');", NULL, NULL, NULL);
    sqlite3_close(db);
    memset(&g_test_sfo, 0, sizeof(g_test_sfo));
    snprintf(g_test_sfo.title_id, sizeof(g_test_sfo.title_id), T);
    snprintf(g_test_sfo.dir_name, sizeof(g_test_sfo.dir_name), "SAVE0");
    snprintf(g_test_sfo.main_title, sizeof(g_test_sfo.main_title), "Test Game");
    g_test_sfo.account_id = 0x1111;
    g_test_sfo.blocks = 96;

    /* Two archived generations. */
    mkdirs(D);
    write_file(D "/" SAVE, "version-1");
    chmod(D "/" SAVE, 0600);
    ls_protect_title(T, "game_close", iobuf);
    latest_archive(q1, sizeof(q1), c1, &g1);
    write_file(D "/" SAVE, "version-2");
    bump(D "/" SAVE);
    ls_protect_title(T, "game_close", iobuf);
    latest_archive(q2, sizeof(q2), c2, &g2);
    drop_jobs();
    CHECK(g1 > 0 && g2 > g1, "two generations (%lld, %lld)", (long long)g1, (long long)g2);

    /* ZIP reader on a real Shallot-format archive. */
    {
        char p[MAX_PATH_LEN], sha[65]; ls_queue_row_t q; ls_queue_get(q1, &q);
        snprintf(p, sizeof(p), "%s/%s", LS_QUEUE_DIR, q.filename);
        CHECK(ls_zip_extract(p, T "/" SAVE, LS_BASE_DIR "/x.img", sha, NULL, iobuf, err, sizeof(err)) == 0 &&
              strcmp(sha, c1) == 0, "zip entry extracted, hash = generation (%s)", err);
        CHECK(ls_zip_extract(p, T "/nope", LS_BASE_DIR "/y.img", sha, NULL, iobuf, err, sizeof(err)) < 0 &&
              strstr(err, "not in archive"), "missing entry reported");
        /* Same archive, but the entry marked as deflated in the central directory. */
        char cp[MAX_PATH_LEN]; snprintf(cp, sizeof(cp), "%s/compressed.zip", LS_BASE_DIR);
        char cmd[2 * MAX_PATH_LEN]; snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", p, cp);
        if (system(cmd) != 0) printf("cp failed\n");
        int fd = open(cp, O_RDWR); struct stat st; fstat(fd, &st);
        uint8_t eocd[22]; pread(fd, eocd, 22, st.st_size - 22);
        uint32_t cdo = ls_le32(eocd + 16); uint8_t eight = 8;
        pwrite(fd, &eight, 1, cdo + 10); close(fd);
        CHECK(ls_zip_extract(cp, T "/" SAVE, LS_BASE_DIR "/z.img", sha, NULL, iobuf, err, sizeof(err)) < 0 &&
              strstr(err, "compressed"), "compressed entries refused (%s)", err);
    }

    /* 1. Restore the older generation. */
    sysdb_mtime(m0, sizeof(m0));
    CHECK(restore_queue(q1, c1, g1, iobuf, err, sizeof(err)) == 0, "request accepted (%s)", err);
    CHECK(state_is("done"), "restore done (%s)", g_ls_restore.message);
    read_file(D "/" SAVE, buf, sizeof(buf));
    CHECK(strcmp(buf, "version-1") == 0, "save now holds version 1 (%s)", buf);
    struct stat st; stat(D "/" SAVE, &st);
    CHECK((st.st_mode & 0777) == 0600, "file mode preserved (%o)", st.st_mode & 0777);
    ls_save_ref_t ref; memset(&ref, 0, sizeof(ref));
    snprintf(ref.user_id, sizeof(ref.user_id), U); snprintf(ref.title_id, sizeof(ref.title_id), T);
    snprintf(ref.save_name, sizeof(ref.save_name), SAVE); snprintf(ref.path, sizeof(ref.path), D "/" SAVE);
    ls_save_row_t row; ls_saves_get(&ref, &row);
    CHECK(row.current_gen_id == g1 && strcmp(row.state, "protected") == 0, "bookkeeping: current = gen %lld", (long long)row.current_gen_id);
    CHECK(g_ls_restore.rollback_gen == g2, "previous version recorded as rollback gen %lld", (long long)g_ls_restore.rollback_gen);
    sysdb_mtime(m1, sizeof(m1));
    CHECK(strcmp(m0, m1) != 0, "savedata.db mtime refreshed (%s -> %s)", m0, m1);
    CHECK(ev_count_code("RESTORE_STAGED") == 1 && ev_count_code("ROLLBACK_CREATED") == 1 &&
          ev_count_code("RESTORE_STARTED") == 1 && ev_count_code("RESTORE_VERIFIED") == 1, "restore events");
    CHECK(!g_ls_rjob.active, "job slot released");
    {
        char wd[MAX_PATH_LEN]; snprintf(wd, sizeof(wd), "%s/%s", LS_RESTORE_DIR, g_ls_restore.id);
        CHECK(!file_exists(wd), "staging cleaned up on success");
    }
    /* The sanity check sees a healthy save afterwards (no change pending). */
    ls_sanity_report_t sr; ls_sanity_title(T, &sr);
    CHECK(sr.result == LS_ST_HEALTHY || sr.result == LS_ST_KNOWN_REGRESSION, "sanity after restore: %s", ls_status_name(sr.result));
    ls_sanity_free(&sr);

    /* 2. Unprotected current change is archived before it is overwritten. */
    write_file(D "/" SAVE, "version-3-unsaved");
    bump(D "/" SAVE);
    CHECK(restore_queue(q2, c2, g2, iobuf, err, sizeof(err)) == 0 && state_is("done"), "restore gen 2 (%s)", g_ls_restore.message);
    read_file(D "/" SAVE, buf, sizeof(buf));
    CHECK(strcmp(buf, "version-2") == 0, "save holds version 2");
    CHECK(q_int("SELECT COUNT(*) FROM queue WHERE reason='pre_restore'") == 1, "version 3 archived as pre_restore");

    /* 3. Same content again: nothing changes. */
    CHECK(restore_queue(q2, c2, g2, iobuf, err, sizeof(err)) == 0 && state_is("done") &&
          strstr(g_ls_restore.message, "already"), "identical content: no change (%s)", g_ls_restore.message);

    /* 4. Archive of another account (matches neither the user nor the current save): refused, untouched. */
    g_test_sfo.account_id = 0x2222; g_test_live_aid_set = 1; g_test_live_aid = 0x1111;
    CHECK(restore_queue(q1, c1, g1, iobuf, err, sizeof(err)) == 0 && state_is("failed") &&
          strstr(g_ls_restore.message, "another account"), "other account refused (%s)", g_ls_restore.message);
    read_file(D "/" SAVE, buf, sizeof(buf));
    CHECK(strcmp(buf, "version-2") == 0, "save untouched after refusal");
    {
        char wd[MAX_PATH_LEN]; snprintf(wd, sizeof(wd), "%s/%s", LS_RESTORE_DIR, g_ls_restore.id);
        CHECK(!file_exists(wd), "validation refusal (nothing changed) cleans its staging");
    }
    CHECK(!g_ls_rjob.active, "slot released after failure");
    g_test_sfo.account_id = 0x1111; g_test_live_aid_set = 0;

    /* 4b. Registry lookup fails: savedata.db supplies the account id. */
    g_test_aid = 0;
    db = NULL; sqlite3_open(DBD "/savedata.db", &db);
    sqlite3_exec(db, "INSERT INTO savedata(title_id, dir_name, account_id) VALUES('PPSA99999','X',4369),"
                     "('PPSA99998','Y',4369),('PPSA99997','Z',8738);", NULL, NULL, NULL);
    sqlite3_close(db);
    restore_queue(q2, c2, g2, iobuf, err, sizeof(err));
    CHECK(state_is("done") && ev_count_code("RESTORE_ACCOUNT") == 1, "account from savedata.db majority (%s)", g_ls_restore.message);
    db = NULL; sqlite3_open(DBD "/savedata.db", &db);
    sqlite3_exec(db, "DELETE FROM savedata WHERE title_id = 'PPSA99997';UPDATE savedata SET account_id = 8738 WHERE title_id = 'PPSA99998';", NULL, NULL, NULL);
    sqlite3_close(db);
    restore_queue(q2, c2, g2, iobuf, err, sizeof(err));
    CHECK(state_is("done") && ev_count_code("RESTORE_ACCOUNT") == 2 && strstr(g_test_last_mounted, "/check_live/"), "no majority: current save decides (%s)", g_ls_restore.message);
    db = NULL; sqlite3_open(DBD "/savedata.db", &db);
    sqlite3_exec(db, "UPDATE savedata SET account_id = 4369 WHERE title_id = '" T "' AND dir_name = 'SAVE0';", NULL, NULL, NULL);
    sqlite3_close(db);
    restore_queue(q2, c2, g2, iobuf, err, sizeof(err));
    CHECK(state_is("done") && ev_count_code("RESTORE_ACCOUNT") == 3, "account from the save's own row (%s)", g_ls_restore.message);
    db = NULL; sqlite3_open(DBD "/savedata.db", &db);
    sqlite3_exec(db, "DELETE FROM savedata WHERE title_id LIKE 'PPSA9999%'; UPDATE savedata SET account_id = NULL;", NULL, NULL, NULL);
    sqlite3_close(db);
    /* 4c. Neither registry nor savedata.db (no PSN account): the current save's account decides. */
    restore_queue(q2, c2, g2, iobuf, err, sizeof(err));
    CHECK(state_is("done") && ev_count_code("RESTORE_ACCOUNT") == 4 && strstr(g_test_last_mounted, "/check_live/"),
          "account from the current save (%s)", g_ls_restore.message);
    g_test_live_aid_set = 1; g_test_live_aid = 0x2222;
    restore_queue(q2, c2, g2, iobuf, err, sizeof(err));
    CHECK(state_is("failed") && strstr(g_ls_restore.message, "another account"), "current save of another account: refused (%s)",
          g_ls_restore.message);
    read_file(D "/" SAVE, buf, sizeof(buf));
    CHECK(strcmp(buf, "version-2") == 0, "save untouched");
    g_test_live_aid_set = 0;
    g_test_aid = 0x1111;
    /* 4e. Archive written before activation (account 0): re-signed to the user's account and placed. */
    {
        char before[256], after[256];
        read_file(D "/" SAVE, before, sizeof(before));
        g_test_sfo.account_id = 0;
        g_test_live_aid_set = 1; g_test_live_aid = 0x1111;
        restore_queue(q1, c1, g1, iobuf, err, sizeof(err));
        g_test_live_aid_set = 0;
        g_test_sfo.account_id = 0x1111;
        read_file(D "/" SAVE, after, sizeof(after));
        CHECK(state_is("done") && ev_count_code("RESTORE_RESIGNED") == 1, "account-0 archive re-signed (%s)", g_ls_restore.message);
        CHECK(strncmp(after, "version-1|RESIGNED=1111", 23) == 0, "re-signed image placed (%s)", after);
        CHECK(ev_count_code("RESTORE_VERIFIED") >= 1, "verified");
        /* Back to the previous content (archived before the restore as pre_restore). */
        (void)before;
    }
    /* 4d. Account id changed (offline activation): archive and current save carry the old one. */
    g_test_aid = 0x3333;
    int64_t acc0 = ev_count_code("RESTORE_ACCOUNT");
    restore_queue(q2, c2, g2, iobuf, err, sizeof(err));
    CHECK(state_is("done") && ev_count_code("RESTORE_ACCOUNT") == acc0 + 1, "old account matching the current save accepted (%s)",
          g_ls_restore.message);
    g_test_live_aid_set = 1; g_test_live_aid = 0x3333;
    restore_queue(q2, c2, g2, iobuf, err, sizeof(err));
    CHECK(state_is("failed") && strstr(g_ls_restore.message, "another account"), "matches neither: refused (%s)",
          g_ls_restore.message);
    g_test_live_aid_set = 0;
    g_test_aid = 0x1111;

    /* 5. Wrong title / save dir in param.sfo. */
    snprintf(g_test_sfo.dir_name, sizeof(g_test_sfo.dir_name), "OTHER");
    restore_queue(q1, c1, g1, iobuf, err, sizeof(err));
    CHECK(state_is("failed") && strstr(g_ls_restore.message, "save directory"), "wrong save dir refused");
    snprintf(g_test_sfo.dir_name, sizeof(g_test_sfo.dir_name), "SAVE0");

    /* 6. Expected-hash mismatch stops at staging. */
    restore_queue(q1, c2, g1, iobuf, err, sizeof(err));
    CHECK(state_is("failed") && strstr(g_ls_restore.message, "does not match"), "hash mismatch refused");

    /* 7. A save mounted in the browser: refused at request time, mount left alone. */
    g_mounted = 1;
    CHECK(restore_queue(q1, c1, g1, iobuf, err, sizeof(err)) == -6 && strstr(err, "mounted"), "browser mount refused up front (%s)", err);
    CHECK(g_mounted == 1 && !g_ls_rjob.active, "browser mount left alone, slot not taken");
    g_mounted = 0;
    /* ...and if it appears between request and run, validation still refuses; nothing kept. */
    {
        ls_queue_row_t q; ls_queue_get(q1, &q);
        char path[MAX_PATH_LEN]; snprintf(path, sizeof(path), "%s/%s", LS_QUEUE_DIR, q.filename);
        CHECK(ls_restore_request("queue", U, T, SAVE, path, c1, g1, err, sizeof(err)) == 0, "queued");
        drop_jobs();
        g_mounted = 1;
        ls_restore_run(iobuf);
        g_mounted = 0;
        CHECK(state_is("failed") && strstr(g_ls_restore.message, "mounted"), "mount during run refused");
        char wd[MAX_PATH_LEN]; snprintf(wd, sizeof(wd), "%s/%s", LS_RESTORE_DIR, g_ls_restore.id);
        CHECK(!file_exists(wd), "refusal before any change leaves no staging");
    }

    /* 8. Image that does not mount. */
    g_test_mount_rc = -5;
    restore_queue(q1, c1, g1, iobuf, err, sizeof(err));
    CHECK(state_is("failed") && strstr(g_ls_restore.message, "does not mount"), "unmountable image refused");
    g_test_mount_rc = 0;
    CHECK(strstr(g_test_last_mounted, "/check/") != NULL, "only the scratch copy is ever mounted (%s)", g_test_last_mounted);

    /* 9. Title running: refused up front; started meanwhile: aborted before the swap. */
    g_running = 1; g_running_title = T;
    CHECK(restore_queue(q1, c1, g1, iobuf, err, sizeof(err)) == -2, "running title refused (%s)", err);
    g_running = 0;
    {
        ls_queue_row_t q; ls_queue_get(q1, &q);
        char path[MAX_PATH_LEN]; snprintf(path, sizeof(path), "%s/%s", LS_QUEUE_DIR, q.filename);
        CHECK(ls_restore_request("queue", U, T, SAVE, path, c1, g1, err, sizeof(err)) == 0, "queued");
        drop_jobs();
        g_running = 1;
        ls_restore_run(iobuf);
        g_running = 0;
        CHECK(state_is("failed") && strstr(g_ls_restore.message, "was started"), "started meanwhile => aborted");
        read_file(D "/" SAVE, buf, sizeof(buf));
        CHECK(strcmp(buf, "version-2") == 0, "save untouched");
    }

    /* 10. Another reconcile job holds the slot. */
    ls_rjob_claim("catalogue_full", "", 0);
    CHECK(restore_queue(q1, c1, g1, iobuf, err, sizeof(err)) == -4, "busy slot refused (%s)", err);
    ls_rjob_finish("completed", "");

    /* 11. Save deleted on the PS5 (file + DB row): re-created. */
    unlink(D "/" SAVE);
    db = NULL; sqlite3_open(DBD "/savedata.db", &db);
    sqlite3_exec(db, "DELETE FROM savedata", NULL, NULL, NULL); sqlite3_close(db);
    CHECK(restore_queue(q1, c1, g1, iobuf, err, sizeof(err)) == 0 && state_is("done"), "re-create (%s)", g_ls_restore.message);
    read_file(D "/" SAVE, buf, sizeof(buf));
    stat(D "/" SAVE, &st);
    CHECK(strcmp(buf, "version-1") == 0 && (st.st_mode & 0777) == 0644, "re-created with upstream import mode");
    sysdb_mtime(m1, sizeof(m1));
    CHECK(m1[0] != 0, "savedata.db row inserted");

    free(iobuf);
    return host_finish();
}
