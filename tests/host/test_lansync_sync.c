/*
 * Host tests for stage L: PC manifest storage/resolution and the per-save
 * sync classification (SYNCED, PS5_NEWER, PC_NEWER, CONFLICT,
 * KNOWN_REGRESSION, MISSING_ON_PC, MISSING_ON_PS5, NO_PC_DATA).
 */
#include "host_worker_stubs.h"

#define T  "PPSA01234"
#define D  LS_USER_HOME "/10000000/savedata_prospero/" T

static void bump(const char *path) {
    struct timespec ts[2];
    clock_gettime(CLOCK_REALTIME, &ts[0]);
    ts[0].tv_sec += 100 + (rand() % 50); ts[1] = ts[0];
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
static void drop_jobs(void) {
    pthread_mutex_lock(&g_ls_job_mtx);
    while (g_ls_jobs) { ls_job_t *j = g_ls_jobs; g_ls_jobs = j->next; free(j); }
    pthread_mutex_unlock(&g_ls_job_mtx);
}
/* Archive entry for the PS5 generation stored in queue row `qid`. */
static void entry_for(char *buf, size_t n, const char *qid) {
    char sql[300], ash[80], csh[80], save[64];
    snprintf(sql, sizeof(sql), "SELECT sha256 FROM queue WHERE id='%s'", qid); q_text(sql, ash, sizeof(ash));
    snprintf(sql, sizeof(sql), "SELECT g.sha256 FROM generations g JOIN queue q ON q.gen_id=g.gen_id WHERE q.id='%s'", qid);
    q_text(sql, csh, sizeof(csh));
    snprintf(sql, sizeof(sql), "SELECT save_name FROM queue WHERE id='%s'", qid); q_text(sql, save, sizeof(save));
    snprintf(buf, n, "{\"queue_id\": \"%s\", \"archive_sha256\": \"%s\", \"container_sha256\": \"%s\", "
             "\"title_id\": \"" T "\", \"save_name\": \"%s\", \"user_id\": \"10000000\", \"created_at\": 1}",
             qid, ash, csh, save);
}
static int manifest(const char *entries) {
    char body[8192];
    snprintf(body, sizeof(body), "{\"archives\": [%s]}", entries);
    return ls_pc_manifest_store(T, body, NULL, NULL);
}
/* sync_state of save A in the compare report. */
static void state_of(const char *save, char *out, size_t n) {
    char *j = ls_compare_title_json(T);
    out[0] = 0;
    char pat[300];
    snprintf(pat, sizeof(pat), "\"save_name\":\"%s\",\"sync_state\":\"", save);
    char *p = j ? strstr(j, pat) : NULL;
    if (p) { p += strlen(pat); size_t k = 0; while (p[k] && p[k] != '"' && k + 1 < n) { out[k] = p[k]; k++; } out[k] = 0; }
    free(j);
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    CHECK(g_ls_schema >= 5, "schema v5");
    char *iobuf = malloc(BUF_SIZE);
    char st[40], e1[1024], e2[1024], all[4096], q1[200], q2[200];

    mkdirs(D);
    write_file(D "/A", "a1");
    ls_protect_title(T, "game_close", iobuf);
    q_text("SELECT id FROM queue WHERE save_name='A' ORDER BY rowid DESC", q1, sizeof(q1));
    write_file(D "/A", "a2"); bump(D "/A");
    ls_protect_title(T, "game_close", iobuf);
    q_text("SELECT id FROM queue WHERE save_name='A' ORDER BY rowid DESC", q2, sizeof(q2));
    drop_jobs();
    entry_for(e1, sizeof(e1), q1);
    entry_for(e2, sizeof(e2), q2);

    /* 1. Nothing reported yet. */
    state_of("A", st, sizeof(st));
    CHECK(strcmp(st, "NO_PC_DATA") == 0, "no report => NO_PC_DATA (%s)", st);

    /* 2. PC holds both generations => SYNCED; PC acks recovered. */
    snprintf(all, sizeof(all), "%s, %s", e1, e2);
    int fo = 0, rep = 0;
    char body[8192];
    snprintf(body, sizeof(body), "{\"archives\": [%s]}", all);
    CHECK(ls_pc_manifest_store(T, body, &fo, &rep) == 2 && fo == 0 && rep == 2, "stored 2, foreign 0, acks recovered 2 (%d/%d)", fo, rep);
    state_of("A", st, sizeof(st));
    CHECK(strcmp(st, "SYNCED") == 0, "SYNCED (%s)", st);

    /* 3. PC only holds the older one => PS5_NEWER. */
    manifest(e1);
    state_of("A", st, sizeof(st));
    CHECK(strcmp(st, "PS5_NEWER") == 0, "PS5_NEWER (%s)", st);

    /* 4. Unprotected PS5 change, PC at latest => PS5_NEWER. */
    manifest(all);
    write_file(D "/A", "a3-unsaved"); bump(D "/A");
    state_of("A", st, sizeof(st));
    CHECK(strcmp(st, "PS5_NEWER") == 0, "unprotected change => PS5_NEWER (%s)", st);

    /* 5. ... and the PC also got a newer version from elsewhere => CONFLICT. */
    char foreign[600];
    snprintf(foreign, sizeof(foreign), "{\"archive_sha256\": \"%064d\", \"container_sha256\": \"%064d\", \"title_id\": \"" T "\","
             " \"save_name\": \"A\", \"user_id\": \"\", \"created_at\": %lld, \"origin\": \"external\"}", 7, 8,
             (long long)time(NULL) + 1000);
    snprintf(all, sizeof(all), "%s, %s, %s", e1, e2, foreign);
    snprintf(body, sizeof(body), "{\"archives\": [%s]}", all);
    CHECK(ls_pc_manifest_store(T, body, &fo, &rep) == 3 && fo == 1, "foreign entry recognized");
    state_of("A", st, sizeof(st));
    CHECK(strcmp(st, "CONFLICT") == 0, "both changed => CONFLICT (%s)", st);

    /* 6. PS5 back at gen 2 content (unchanged since common) + PC newer => PC_NEWER. */
    write_file(D "/A", "a2"); bump(D "/A");
    ls_protect_title(T, "game_close", iobuf);    /* re-identifies gen 2, no new archive */
    drop_jobs();
    state_of("A", st, sizeof(st));
    CHECK(strcmp(st, "PC_NEWER") == 0, "PC_NEWER (%s)", st);

    /* 7. Panic regression: PS5 at gen 1, PC holds gen 2 => KNOWN_REGRESSION. */
    snprintf(all, sizeof(all), "%s, %s", e1, e2);
    manifest(all);
    write_file(D "/A", "a1"); bump(D "/A");
    ls_protect_title(T, "game_close", iobuf);
    drop_jobs();
    state_of("A", st, sizeof(st));
    CHECK(strcmp(st, "KNOWN_REGRESSION") == 0, "KNOWN_REGRESSION (%s)", st);

    /* 8. PC holds only versions this PS5 never made => CONFLICT (no common ancestor). */
    manifest(foreign);
    state_of("A", st, sizeof(st));
    CHECK(strcmp(st, "CONFLICT") == 0, "no common ancestor => CONFLICT (%s)", st);

    /* 9. Resolution by container hash only (no queue id) and MISSING_ON_PC for a new save. */
    char csh[80], ash[80], byhash[600];
    char sql[300];
    snprintf(sql, sizeof(sql), "SELECT g.sha256 FROM generations g JOIN queue q ON q.gen_id=g.gen_id WHERE q.id='%s'", q2);
    q_text(sql, csh, sizeof(csh));
    snprintf(sql, sizeof(sql), "SELECT sha256 FROM queue WHERE id='%s'", q2); q_text(sql, ash, sizeof(ash));
    snprintf(byhash, sizeof(byhash), "{\"archive_sha256\": \"%s\", \"container_sha256\": \"%s\", \"title_id\": \"" T "\","
             " \"save_name\": \"A\", \"user_id\": \"10000000\", \"created_at\": 1}", ash, csh);
    snprintf(body, sizeof(body), "{\"archives\": [%s]}", byhash);
    CHECK(ls_pc_manifest_store(T, body, &fo, &rep) == 1 && fo == 0, "resolved by container hash");
    write_file(D "/B", "b1");
    ls_protect_title(T, "game_close", iobuf);
    drop_jobs();
    state_of("B", st, sizeof(st));
    CHECK(strcmp(st, "MISSING_ON_PC") == 0, "new save not on PC => MISSING_ON_PC (%s)", st);

    /* 10. Save deleted on PS5, PC still has it => MISSING_ON_PS5. */
    manifest(all);
    unlink(D "/A");
    state_of("A", st, sizeof(st));
    CHECK(strcmp(st, "MISSING_ON_PS5") == 0, "MISSING_ON_PS5 (%s)", st);

    /* 11. Garbage is ignored. */
    CHECK(manifest("{\"archive_sha256\": \"nothex\", \"save_name\": \"A\"},"
                   "{\"archive_sha256\": \"0000000000000000000000000000000000000000000000000000000000000001\","
                   " \"save_name\": \"A\", \"title_id\": \"PPSA99999\"}") == 0, "invalid / other-title entries skipped");
    CHECK(ls_pc_manifest_store(T, "{\"nope\": 1}", NULL, NULL) == -1, "missing archives array rejected");

    /* 12. Full reconcile reports pc_compared and sync states. */
    write_file(D "/A", "a2"); bump(D "/A");
    manifest(all);
    ls_full_report_t fr;
    ls_rjob_claim("title_full", T, 0);
    ls_full_title(T, &fr, iobuf);
    ls_rjob_finish("completed", "");
    char *fj = ls_full_json(&fr, 0, 0);
    CHECK(fr.pc_compared && fj && strstr(fj, "\"pc_compared\":true") && strstr(fj, "\"sync_state\":\"SYNCED\""),
          "full reconcile includes PC comparison");
    free(fj);
    free(fr.items);
    CHECK(ev_count_code("SYNC_CONFLICT_DETECTED") >= 1, "conflict event logged");

    free(iobuf);
    return host_finish();
}
