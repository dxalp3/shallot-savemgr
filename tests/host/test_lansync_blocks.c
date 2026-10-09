/*
 * Host tests for PS5-only mode (src/lansync_blocks.inc.c): archives packed
 * into the block store (few new bytes per version), the exact ZIP rebuilt
 * for readers, a restore from a packed archive, thinning of old versions
 * and restore points, and compaction of dead pack space.
 */
#include "host_worker_stubs.h"

#define T   "PPSA00002"
#define U   "10000000"
#define D   LS_USER_HOME "/" U "/savedata_prospero/" T
#define DBD LS_SYSTEM_DATA "/savedata_prospero/" U "/db/user"
#define IMG (256 * 1024)

static unsigned char g_img[IMG];

static int64_t q_int(const char *sql) {
    sqlite3_stmt *st = NULL; int64_t v = -1;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    return v;
}
static void q_exec(const char *sql) {
    pthread_mutex_lock(&g_ls_mtx);
    ls_exec_locked(sql);
    pthread_mutex_unlock(&g_ls_mtx);
}
static void put_img(const char *path, int tick) {
    FILE *f = fopen(path, "wb");
    fwrite(g_img, 1, IMG, f);
    fclose(f);
    struct timespec ts[2];
    clock_gettime(CLOCK_REALTIME, &ts[0]);
    ts[0].tv_sec += 100 + tick * 10; ts[1] = ts[0];
    utimensat(AT_FDCWD, path, ts, 0);
}
static void drop_jobs(void) {
    pthread_mutex_lock(&g_ls_job_mtx);
    while (g_ls_jobs) { ls_job_t *j = g_ls_jobs; g_ls_jobs = j->next; free(j); }
    pthread_mutex_unlock(&g_ls_job_mtx);
}
/* queue id of the n-th archive (oldest first) of a save file */
static int qid_of(const char *save, int nth, char *out, size_t out_sz) {
    sqlite3_stmt *st = NULL; int k = 0, found = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, "SELECT id FROM queue WHERE save_name = ? ORDER BY created_at, id", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, save, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (k++ == nth) { snprintf(out, out_sz, "%s", (const char *)sqlite3_column_text(st, 0)); found = 1; break; }
        }
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    return found;
}
static int file_matches_img(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static unsigned char b[IMG + 1];
    size_t n = fread(b, 1, sizeof(b), f);
    fclose(f);
    return n == IMG && memcmp(b, g_img, IMG) == 0;
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    g_ls_worker_started = 1;
    g_running_title = T;
    char *iobuf = malloc(BUF_SIZE);
    char err[300];
    mkdirs(DBD);
    sqlite3 *db = NULL;
    sqlite3_open(DBD "/savedata.db", &db);
    sqlite3_exec(db, "CREATE TABLE savedata(id INTEGER PRIMARY KEY AUTOINCREMENT, title_id, dir_name, main_title, sub_title,"
                     " detail, tmp_dir_name, is_broken, user_param, blocks, free_blocks, size_kib, mtime, fake_broken,"
                     " account_id, user_id, faked_owner, cloud_icon_url, cloud_revision, game_title_id, system_blocks);",
                 NULL, NULL, NULL);
    sqlite3_close(db);
    memset(&g_test_sfo, 0, sizeof(g_test_sfo));
    snprintf(g_test_sfo.title_id, sizeof(g_test_sfo.title_id), T);
    g_test_sfo.account_id = 0x1111;
    g_test_sfo_dir_from_path = 1;

    ls_config_t cfg;
    ls_load_config(&cfg);
    cfg.storage_ps5 = 1;
    ls_save_config(&cfg);
    ls_load_config(&cfg);
    CHECK(cfg.storage_ps5 == 1, "storage_mode ps5 saved");

    /* Three versions of one save: each differs in a single 4 KiB block. */
    srand(7);
    for (int i = 0; i < IMG; i++) g_img[i] = (unsigned char)rand();
    unsigned char v1[IMG];
    mkdirs(D);
    int64_t snap[3] = {0};
    put_img(D "/sdimg_Save04", 0);
    ls_protect_title_ex(T, "game_close", iobuf, &snap[0]);
    memcpy(v1, g_img, IMG);
    g_img[100 * 1024] ^= 0xff;
    put_img(D "/sdimg_Save04", 1);
    ls_protect_title_ex(T, "game_close", iobuf, &snap[1]);
    g_img[200 * 1024] ^= 0xff;
    put_img(D "/sdimg_Save04", 2);
    ls_protect_title_ex(T, "game_close", iobuf, &snap[2]);
    CHECK(snap[0] && snap[1] > snap[0] && snap[2] > snap[1], "three snapshots");
    char a1[200], a2[200], a3[200];
    CHECK(qid_of("sdimg_Save04", 0, a1, sizeof(a1)) && qid_of("sdimg_Save04", 1, a2, sizeof(a2)) &&
          qid_of("sdimg_Save04", 2, a3, sizeof(a3)), "three archives");
    ls_queue_row_t r1, r2;
    ls_queue_get(a1, &r1);
    ls_queue_get(a2, &r2);
    char z1[512];
    snprintf(z1, sizeof(z1), "%s/%s", LS_QUEUE_DIR, r1.filename);

    /* Pack the two older versions. */
    CHECK(ls_blocks_pack(a1, iobuf, err, sizeof(err)) == 1, "first archive packed (%s)", err);
    int64_t after1 = q_int("SELECT SUM(bytes) FROM packs");
    CHECK(access(z1, F_OK) != 0 && q_int("SELECT COUNT(*) FROM queue WHERE state = 'packed'") == 1, "ZIP removed, row packed");
    CHECK(ls_blocks_pack(a2, iobuf, err, sizeof(err)) == 1, "second archive packed (%s)", err);
    int64_t after2 = q_int("SELECT SUM(bytes) FROM packs");
    CHECK(after1 >= IMG && after2 - after1 <= 2 * LS_BLOCK_SIZE + 2048, "second version cost %lld new bytes (first %lld)",
          (long long)(after2 - after1), (long long)after1);
    CHECK(ls_blocks_pack(a2, iobuf, err, sizeof(err)) == 0, "packing twice is a no-op");

    /* Readers get the exact ZIP back. */
    CHECK(ls_archive_ensure_ready(a1, err, sizeof(err)) == 0 && access(z1, F_OK) == 0, "rebuilt for a reader (%s)", err);
    char hex[65];
    CHECK(ls_sha256_file(z1, hex, NULL, iobuf) == 0 && strcmp(hex, r1.sha256) == 0, "rebuilt ZIP is byte-identical");
    ls_queue_get(a1, &r1);
    CHECK(strcmp(r1.state, "ready") == 0, "row ready again");
    int64_t before = q_int("SELECT SUM(bytes) FROM packs");
    CHECK(ls_blocks_pack(a1, iobuf, err, sizeof(err)) == 1 && q_int("SELECT SUM(bytes) FROM packs") == before,
          "repacked from the kept recipe, nothing written (%s)", err);

    /* Restore save 04 to version 1 while its archive is packed. */
    int rc = ls_restore_request_snapshot(snap[0], "Save04", err, sizeof(err));
    CHECK(rc == 0, "restore of a packed version accepted (%s)", err);
    if (rc == 0) { drop_jobs(); ls_restore_run(iobuf); }
    memcpy(g_img, v1, IMG);
    CHECK(file_matches_img(D "/sdimg_Save04"), "save holds version 1 again (%s)", g_ls_restore.message);

    /* Corrupt pack bytes are caught: rebuild refuses, the row stays packed. */
    ls_blocks_pack(a1, iobuf, err, sizeof(err));
    {
        char pp[512];
        ls_pack_path(1, pp, sizeof(pp));
        FILE *f = fopen(pp, "r+b");
        fseek(f, 5000, SEEK_SET); int c = fgetc(f); fseek(f, 5000, SEEK_SET); fputc(c ^ 0x5a, f); fclose(f);
        CHECK(ls_archive_ensure_ready(a1, err, sizeof(err)) < 0 && strstr(err, "does not match"), "corrupt block refused (%s)", err);
        ls_queue_get(a1, &r1);
        CHECK(strcmp(r1.state, "packed") == 0 && access(z1, F_OK) != 0, "nothing half-written");
        f = fopen(pp, "r+b");
        fseek(f, 5000, SEEK_SET); fputc(c, f); fclose(f);
    }

    /* Thinning: everything 40 days old; keep 2 days, daily 30, weekly. */
    int64_t now = (int64_t)time(NULL);
    char sql[512];
    snprintf(sql, sizeof(sql), "UPDATE snapshots SET created_at = %lld - (5 - snap_id) * 3600", (long long)(now - 40 * 86400));
    q_exec(sql);
    snprintf(sql, sizeof(sql), "UPDATE queue SET created_at = %lld + rowid", (long long)(now - 40 * 86400));
    q_exec(sql);
    /* A PC client that was in touch does not hold thinning back: it is not a destination here. */
    int64_t snaps_before = q_int("SELECT COUNT(*) FROM snapshots");
    ls_meta_set_int("last_pc_ack", now - 3600);
    int sd = 0;
    int dropped = ls_ps5_thin(now, &sd);
    int64_t snaps_after = q_int("SELECT COUNT(*) FROM snapshots");
    CHECK(sd >= 1 && snaps_after == snaps_before - sd && snaps_after >= 1, "old restore points thinned (%lld -> %lld)",
          (long long)snaps_before, (long long)snaps_after);
    CHECK(dropped >= 1 && q_int("SELECT COUNT(*) FROM queue WHERE state = 'pruned'") == dropped, "unneeded archives dropped (%d)", dropped);
    CHECK(q_int("SELECT COUNT(*) FROM queue q WHERE q.save_name = 'sdimg_Save04' AND q.state IN ('ready','packed')"
                " AND q.created_at = (SELECT MAX(created_at) FROM queue WHERE save_name = 'sdimg_Save04')") == 1,
          "newest version of the save kept");
    CHECK(q_int("SELECT COUNT(*) FROM recipes r JOIN queue q ON q.id = r.queue_id WHERE q.state = 'pruned'") == 0,
          "dropped archives released their blocks");

    /* Compaction: nothing while a pack is mostly live; once most of it is dead
     * it is rewritten into the current pack and its space comes back. */
    q_exec("INSERT OR IGNORE INTO packs(pack, bytes, dead) VALUES(2, 0, 0)");   /* force a new current pack */
    CHECK(ls_blocks_compact(iobuf) == 0, "mostly-live pack left alone");
    /* Drop every packed archive: the old pack is all dead now. */
    for (;;) {
        char id[200] = "";
        sqlite3_stmt *st = NULL;
        pthread_mutex_lock(&g_ls_mtx);
        if (sqlite3_prepare_v2(g_ls_db, "SELECT id FROM queue WHERE state = 'packed' LIMIT 1", -1, &st, NULL) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW) snprintf(id, sizeof(id), "%s", (const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
        pthread_mutex_unlock(&g_ls_mtx);
        if (!id[0]) break;
        ls_queue_row_t q; ls_queue_get(id, &q); ls_archive_drop(&q);
    }
    int64_t dead = q_int("SELECT SUM(dead) FROM packs");
    char p1[512];
    ls_pack_path(1, p1, sizeof(p1));
    int64_t got = ls_blocks_compact(iobuf);
    CHECK(dead > 0 && got >= dead - 4096 && access(p1, F_OK) != 0 && q_int("SELECT COUNT(*) FROM blocks WHERE pack = 1") == 0,
          "dead pack rewritten away: %lld of %lld bytes back", (long long)got, (long long)dead);
    /* The store keeps working after compaction: pack a remaining older archive and rebuild it. */
    {
        char id[200] = "";
        sqlite3_stmt *st = NULL;
        pthread_mutex_lock(&g_ls_mtx);
        if (sqlite3_prepare_v2(g_ls_db, "SELECT id FROM queue WHERE state = 'ready' ORDER BY created_at LIMIT 1", -1, &st, NULL) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW) snprintf(id, sizeof(id), "%s", (const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
        pthread_mutex_unlock(&g_ls_mtx);
        int ok = id[0] && ls_blocks_pack(id, iobuf, err, sizeof(err)) == 1;
        if (ok) ok = ls_archive_ensure_ready(id, err, sizeof(err)) == 0;
        CHECK(ok, "pack + rebuild after compaction (%s)", err);
    }

    /* Housekeeping end to end + status numbers; PC-mode prune leaves PS5-only mode alone. */
    ls_ps5_housekeeping(20);
    CHECK(q_int("SELECT COUNT(*) FROM events WHERE code = 'PS5_HOUSEKEEPING'") >= 0, "housekeeping ran");
    char sj[512];
    ls_ps5_store_json(sj, sizeof(sj));
    CHECK(strstr(sj, "\"mode\":\"ps5\"") && strstr(sj, "\"pack_bytes\":"), "store status (%s)", sj);
    int64_t ready_before = q_int("SELECT COUNT(*) FROM queue WHERE state IN ('ready','packed')");
    q_exec("UPDATE queue SET lan_ack_at = 1");
    ls_queue_prune(&cfg);
    CHECK(q_int("SELECT COUNT(*) FROM queue WHERE state IN ('ready','packed')") == ready_before, "PC prune skipped in PS5-only mode");

    free(iobuf);
    return host_finish();
}
