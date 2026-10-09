/*
 * Host tests for game / save-object snapshots (schema v6): a whole-game
 * backup commits one coherent snapshot of every save object; unchanged state
 * makes no new snapshot; a change mid-backup defers it; per-object backups
 * archive the primary and its backup copy together.
 */
#include "host_worker_stubs.h"

#define T  "PPSA00002"
#define U  "10000000"
#define D  LS_USER_HOME "/" U "/savedata_prospero/" T

static int64_t q_int(const char *sql) {
    sqlite3_stmt *st = NULL; int64_t v = -1;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    return v;
}
static void bump(const char *path, const char *data) {
    write_file(path, data);
    struct timespec ts[2];
    clock_gettime(CLOCK_REALTIME, &ts[0]);
    ts[0].tv_sec += 50 + rand() % 50; ts[1] = ts[0];
    utimensat(AT_FDCWD, path, ts, 0);
}
static int64_t gen_of(int64_t snap, const char *save) {
    char sql[300];
    snprintf(sql, sizeof(sql), "SELECT gen_id FROM snapshot_files WHERE snap_id = %lld AND save_name = '%s'", (long long)snap, save);
    return q_int(sql);
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    CHECK(g_ls_schema >= 6, "schema v6");
    char *iobuf = malloc(BUF_SIZE);

    mkdirs(D);
    write_file(D "/sdimg_Save00", "s0");
    write_file(D "/sdimg_sce_bu_Save00", "s0");
    write_file(D "/sdimg_Save04", "s4");
    write_file(D "/sdimg_sce_bu_Save04", "s4");
    write_file(D "/sdimg_Save05", "s5");

    /* Whole-game backup: one snapshot with every object and file. */
    int64_t snap1 = 0;
    CHECK(ls_protect_title_ex(T, "game_close", iobuf, &snap1) == 0 && snap1 > 0, "game snapshot taken (%lld)", (long long)snap1);
    CHECK(q_int("SELECT objects FROM snapshots WHERE kind='title'") == 3 && q_int("SELECT files FROM snapshots WHERE kind='title'") == 5,
          "3 objects / 5 files");
    CHECK(q_int("SELECT COUNT(*) FROM snapshot_files WHERE object_id='Save04'") == 2, "Save04 has primary + backup copy");
    CHECK(q_int("SELECT COUNT(*) FROM snapshot_files WHERE role='sce_backup'") == 2, "two backup copies");
    CHECK(ev_count_code("SNAPSHOT_CREATED") == 1, "one SNAPSHOT_CREATED");

    /* Nothing changed: same snapshot, no new row. */
    int64_t again = 0;
    ls_protect_title_ex(T, "close_followup", iobuf, &again);
    CHECK(again == snap1 && q_int("SELECT COUNT(*) FROM snapshots") == 1, "unchanged game: no new snapshot");

    /* Save04 (and its backup copy) change: new snapshot, other objects keep their generations. */
    bump(D "/sdimg_Save04", "s4-v2");
    bump(D "/sdimg_sce_bu_Save04", "s4-v2");
    int64_t snap2 = 0;
    ls_protect_title_ex(T, "game_close", iobuf, &snap2);
    CHECK(snap2 > snap1, "new snapshot after a change");
    CHECK(gen_of(snap2, "sdimg_Save00") == gen_of(snap1, "sdimg_Save00") &&
          gen_of(snap2, "sdimg_Save05") == gen_of(snap1, "sdimg_Save05"), "unchanged objects reference the same generations");
    CHECK(gen_of(snap2, "sdimg_Save04") != gen_of(snap1, "sdimg_Save04") &&
          gen_of(snap2, "sdimg_sce_bu_Save04") != gen_of(snap1, "sdimg_sce_bu_Save04"), "Save04 pair moved on together");

    /* An already-handled file changes while a later one is packaged: no snapshot, retry asked for. */
    bump(D "/sdimg_Save05", "s5-v2");
    g_mutate_during_stage = D "/sdimg_Save00";      /* touched while Save05 is zipped */
    int64_t snap3 = 0;
    int left = ls_protect_title_ex(T, "game_close", iobuf, &snap3);
    CHECK(left == 1 && snap3 == 0 && ev_count_code("SNAPSHOT_DEFERRED") == 1, "moved during backup: snapshot deferred (left=%d)", left);
    CHECK(q_int("SELECT COUNT(*) FROM snapshots WHERE kind='title'") == 2, "no partial snapshot stored");
    /* The retry captures the settled state. */
    ls_protect_title_ex(T, "game_close", iobuf, &snap3);
    CHECK(snap3 > snap2, "retry commits the snapshot");

    /* Individual object backup: primary + backup copy as one unit. */
    bump(D "/sdimg_Save04", "s4-v3");
    bump(D "/sdimg_sce_bu_Save04", "s4-v3");
    int64_t before_q = q_int("SELECT COUNT(*) FROM queue");
    int64_t osnap = 0;
    CHECK(ls_protect_object(T, "Save04", "manual_object", iobuf, &osnap) == 0 && osnap > 0, "object snapshot");
    CHECK(q_int("SELECT COUNT(*) FROM queue") == before_q + 2, "both Save04 files archived (and nothing else)");
    CHECK(q_int("SELECT kind = 'object' AND object_id = 'Save04' AND files = 2 FROM snapshots ORDER BY snap_id DESC LIMIT 1") == 1,
          "object snapshot holds the pair");
    CHECK(ls_protect_object(T, "NoSuch", "manual_object", iobuf, NULL) == -1, "unknown object");

    /* Orphan backup copy and a primary without a copy are both captured. */
    write_file(D "/sdimg_sce_bu_Save08", "orphan");
    int64_t snap4 = 0;
    ls_protect_title_ex(T, "game_close", iobuf, &snap4);
    CHECK(q_int("SELECT COUNT(*) FROM snapshot_files WHERE snap_id = (SELECT MAX(snap_id) FROM snapshots WHERE kind='title')"
                " AND object_id = 'Save08' AND role = 'sce_backup'") == 1, "orphan backup copy recorded as its own object");

    free(iobuf);
    return host_finish();
}
