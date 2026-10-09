/*
 * Host test: upgrading a state DB written by the stage-C build (as found on
 * the owner's PS5: user_version 2, saves table without current_gen_id, PC
 * acks present) to schema v3 without losing data, and idempotent reopen.
 */
#include "host_stubs.h"

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

static void close_state_db(void) {
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_close(g_ls_db);
    g_ls_db = NULL;
    pthread_mutex_unlock(&g_ls_mtx);
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    mkdir(LS_STATE_DIR, 0777);

    /* Build a stage-C era DB by hand. */
    sqlite3 *db = NULL;
    sqlite3_open(LS_DB_PATH, &db);
    const char *old_schema =
        "CREATE TABLE meta(key TEXT PRIMARY KEY, value);"
        "CREATE TABLE queue(id TEXT PRIMARY KEY, filename TEXT NOT NULL, title_id TEXT, save_name TEXT,"
        " user_id TEXT, gen_id INTEGER DEFAULT 0, sha256 TEXT, size INTEGER, created_at INTEGER, reason TEXT,"
        " state TEXT NOT NULL, cloud_needed INTEGER DEFAULT 0, lan_ack_at INTEGER DEFAULT 0,"
        " cloud_ack_at INTEGER DEFAULT 0, pc_attempts INTEGER DEFAULT 0, last_error TEXT);"
        "CREATE TABLE ops(id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT, target TEXT, started_at INTEGER,"
        " finished_at INTEGER, status TEXT, detail TEXT);"
        "CREATE TABLE events(id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER, code TEXT, title_id TEXT, detail TEXT);"
        "CREATE TABLE saves(user_id TEXT NOT NULL, title_id TEXT NOT NULL, save_name TEXT NOT NULL,"
        " is_ps4 INTEGER DEFAULT 0, last_seen_meta TEXT, last_seen_at INTEGER DEFAULT 0,"
        " last_packaged_meta TEXT, last_packaged_sha TEXT, last_packaged_at INTEGER DEFAULT 0,"
        " last_queue_id TEXT, state TEXT DEFAULT 'unknown', last_error TEXT,"
        " PRIMARY KEY(user_id, title_id, save_name));"
        "INSERT INTO queue(id, filename, title_id, save_name, user_id, sha256, size, created_at, reason, state, lan_ack_at)"
        " VALUES('q_old','q_old.zip','PPSA15508','sdimg_SAVEFILE00','10000000','aa',1,100,'game_close','ready',555),"
        "       ('q_new','q_new.zip','PPSA15508','sdimg_SAVEFILE0A','10000000','bb',1,200,'game_close','ready',0);"
        "INSERT INTO saves(user_id, title_id, save_name, last_packaged_meta, last_packaged_sha, last_packaged_at,"
        " last_queue_id, state) VALUES"
        " ('10000000','PPSA15508','sdimg_SAVEFILE00','s=1','c0ffee00',100,'q_old','protected'),"
        " ('10000000','PPSA15508','sdimg_SAVEFILE0A','s=1','c0ffee0a',200,'q_new','protected'),"
        " ('10000000','PPSA15508','sdimg_NEVER','s=1','',0,'','changed');"
        "PRAGMA user_version=2;";
    char *msg = NULL;
    CHECK(sqlite3_exec(db, old_schema, NULL, NULL, &msg) == SQLITE_OK, "old schema: %s", msg ? msg : "");
    sqlite3_close(db);

    /* Upgrade. */
    CHECK(host_open_state_db(), "open + migrate");
    CHECK(g_ls_schema == LS_SCHEMA_VERSION, "schema now latest (%d)", g_ls_schema);
    CHECK(q_int("PRAGMA user_version") == LS_SCHEMA_VERSION, "user_version latest");
    CHECK(q_int("SELECT COUNT(*) FROM queue") == 2, "queue rows kept");
    CHECK(q_int("SELECT COUNT(*) FROM saves") == 3, "saves rows kept");
    CHECK(q_int("SELECT COUNT(*) FROM generations") == 2, "two generations backfilled (empty sha skipped)");
    CHECK(q_int("SELECT COUNT(*) FROM saves WHERE current_gen_id > 0") == 2, "current_gen_id set");
    CHECK(q_int("SELECT COUNT(*) FROM queue WHERE gen_id > 0") == 2, "queue linked to generations");
    CHECK(q_int("SELECT pc_acked_at FROM generations WHERE queue_id = 'q_old'") == 555, "PC ack carried over");
    CHECK(q_int("SELECT pc_acked_at FROM generations WHERE queue_id = 'q_new'") == 0, "unacked stays unacked");
    /* v6: the last protected state becomes one game snapshot (legacy backups stay restorable as a set). */
    CHECK(q_int("SELECT COUNT(*) FROM snapshots WHERE title_id = 'PPSA15508' AND kind = 'title' AND reason = 'migrated'") == 1,
          "one migrated game snapshot");
    CHECK(q_int("SELECT files FROM snapshots WHERE reason = 'migrated'") == 2 &&
          q_int("SELECT objects FROM snapshots WHERE reason = 'migrated'") == 2, "two files, two objects (never-packaged skipped)");
    {
        char oid[64] = {0};
        q_text("SELECT object_id || '/' || role FROM snapshot_files WHERE save_name = 'sdimg_SAVEFILE0A'", oid, sizeof(oid));
        CHECK(strcmp(oid, "SAVEFILE0A/primary") == 0, "object id derived in SQL (%s)", oid);
    }

    /* Ack after upgrade propagates to the generation. */
    CHECK(ls_queue_ack("q_new", "pc", "bb") == 0, "ack q_new");
    CHECK(q_int("SELECT pc_acked_at > 0 FROM generations WHERE queue_id = 'q_new'") == 1, "gen ack follows queue ack");

    /* Reopen: nothing re-runs, nothing duplicated. */
    close_state_db();
    CHECK(host_open_state_db(), "reopen");
    CHECK(g_ls_schema == LS_SCHEMA_VERSION, "still latest");
    CHECK(q_int("SELECT COUNT(*) FROM generations") == 2, "no duplicate backfill");

    /* Fresh DB goes straight to v3. */
    close_state_db();
    host_reset_base();
    CHECK(host_open_state_db(), "fresh open");
    CHECK(g_ls_schema == LS_SCHEMA_VERSION && q_int("SELECT COUNT(*) FROM generations") == 0, "fresh db at latest schema");
    CHECK(q_int("SELECT COUNT(*) FROM pragma_table_info('saves') WHERE name = 'current_gen_id'") == 1,
          "fresh saves has current_gen_id");
    return host_finish();
}
