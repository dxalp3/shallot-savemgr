/*
 * Host tests for the savedata.db cross-check (SAVE_NOT_SCANNED): saves the
 * PS5 database lists but the scanner would skip must be reported by the
 * sanity check, full reconcile and startup reconciliation.
 */
#include "host_worker_stubs.h"

#define T1   LS_USER_HOME "/10000000/savedata_prospero/PPSA01234"
#define DBD  LS_SYSTEM_DATA "/savedata_prospero/10000000/db/user"

static ls_save_status_t st_of(const ls_sanity_report_t *r, const char *save) {
    for (int i = 0; i < r->nitems; i++)
        if (strcmp(r->items[i].save_name, save) == 0) return r->items[i].status;
    return LS_ST__COUNT;
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    char *iobuf = malloc(BUF_SIZE);

    mkdirs(T1);
    mkdirs(DBD);
    write_file(T1 "/sdimg_A", "normal");
    write_file(T1 "/sdimg_0000.bin", "digimon-style");   /* accepted since the fix */
    mkdirs(T1 "/sdimg_WEIRD");                          /* listed by the DB, but a directory */
    sqlite3 *db = NULL;
    sqlite3_open(DBD "/savedata.db", &db);
    sqlite3_exec(db,
        "CREATE TABLE savedata(title_id TEXT, dir_name TEXT, mtime TEXT, main_title TEXT);"
        "INSERT INTO savedata VALUES('PPSA01234','A','t','x'),('PPSA01234','0000.bin','t','x'),"
        "('PPSA01234','WEIRD','t','x'),('PPSA01234','GHOST','t','x');", NULL, NULL, NULL);
    sqlite3_close(db);

    /* Filter rule itself. */
    CHECK(ls_save_name_accepted("sdimg_0000.bin", 0) && ls_save_name_accepted("sdimg_A", 1),
          "sdimg_* accepted (PS5 and PS4)");
    CHECK(!ls_save_name_accepted("A.bin", 1) && !ls_save_name_accepted(".hidden", 0) &&
          !ls_save_name_accepted("plainfile", 1), "PS4 companions, dot files, PS4 non-images rejected");

    /* Cross-check. */
    ls_unscanned_t un[8];
    int db_only = 0;
    int n = ls_find_unscanned("PPSA01234", un, 8, &db_only);
    CHECK(n == 1 && strcmp(un[0].save_name, "sdimg_WEIRD") == 0 && strstr(un[0].reason, "regular"),
          "one unscanned (%d)", n);
    CHECK(db_only == 1, "ghost row counted as db_only (%d)", db_only);
    CHECK(ls_find_unscanned(NULL, NULL, 0, NULL) == 1, "all-title query agrees");

    /* Sanity check reports it (title tracked after one protect). */
    ls_protect_title("PPSA01234", "game_close", iobuf);
    ls_sanity_report_t r;
    ls_sanity_title("PPSA01234", &r);
    CHECK(st_of(&r, "sdimg_WEIRD") == LS_ST_NOT_SCANNED, "sanity: SAVE_NOT_SCANNED");
    CHECK(st_of(&r, "sdimg_0000.bin") == LS_ST_HEALTHY, "sanity: .bin save protected and healthy");
    CHECK(r.result == LS_ST_NOT_SCANNED, "title result escalates (%s)", ls_status_name(r.result));
    ls_sanity_free(&r);

    /* Full reconcile reports it too. */
    ls_full_report_t fr;
    ls_rjob_claim("title_full", "PPSA01234", 0);
    ls_full_title("PPSA01234", &fr, iobuf);
    ls_rjob_finish("completed", "");
    int seen = 0;
    for (int i = 0; i < fr.nitems; i++)
        if (fr.items[i].status == LS_ST_NOT_SCANNED) seen++;
    CHECK(seen == 1, "full: one SAVE_NOT_SCANNED (%d)", seen);
    free(fr.items);

    /* Startup reconciliation counts and logs it. */
    ls_reconcile_counts_t c;
    ls_startup_reconcile(&c);
    CHECK(c.not_scanned == 1, "startup not_scanned=1 (%d)", c.not_scanned);
    CHECK(ev_count_code("SAVE_NOT_SCANNED") == 1, "SAVE_NOT_SCANNED event");

    pthread_mutex_lock(&g_ls_job_mtx);
    while (g_ls_jobs) { ls_job_t *j = g_ls_jobs; g_ls_jobs = j->next; free(j); }
    pthread_mutex_unlock(&g_ls_job_mtx);
    free(iobuf);
    return host_finish();
}
