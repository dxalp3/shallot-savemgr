/*
 * Host-side tests for src/lansync_core.inc.c (Linux/WSL, native compiler).
 * Run: sh tests/host/run.sh
 * Stubs the few main.c helpers the core depends on and points all paths at a
 * temp directory.
 */
#include "host_stubs.h"

static int64_t q_count(const char *where) {
    char sql[256]; snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM queue WHERE %s", where);
    sqlite3_stmt *st = NULL; int64_t n = -1;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    return n;
}
static int64_t ev_count(const char *code) {
    sqlite3_stmt *st = NULL; int64_t n = -1;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, "SELECT COUNT(*) FROM events WHERE code = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, code, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    return n;
}

static void test_sha256(char *iobuf) {
    ls_sha256_t c; char hex[65];
    ls_sha256_init(&c); ls_sha256_final_hex(&c, hex);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0, "empty: %s", hex);
    ls_sha256_init(&c); ls_sha256_update(&c, "abc", 3); ls_sha256_final_hex(&c, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "abc: %s", hex);
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    ls_sha256_init(&c); ls_sha256_update(&c, m, strlen(m)); ls_sha256_final_hex(&c, hex);
    CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0, "448-bit: %s", hex);
    /* one million 'a' via file, exercising multi-block reads */
    char path[512]; snprintf(path, sizeof(path), "%s/million_a", g_base);
    FILE *f = fopen(path, "wb"); for (int i = 0; i < 1000000; i++) fputc('a', f); fclose(f);
    int64_t sz = 0;
    CHECK(ls_sha256_file(path, hex, &sz, iobuf) == 0 && sz == 1000000, "file hash");
    CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0, "1M a: %s", hex);
    unlink(path);
}

static void test_queue_and_ack(char *iobuf) {
    char src[512], err[256] = {0}, id[200] = {0};
    snprintf(src, sizeof(src), "%s/stage.zip", g_base);
    write_file(src, "PK-fake-archive-bytes");
    ls_queue_meta_t m; memset(&m, 0, sizeof(m));
    snprintf(m.title_id, sizeof(m.title_id), "PPSA01234");
    snprintf(m.save_name, sizeof(m.save_name), "SAVE.DATA 01");
    snprintf(m.reason, sizeof(m.reason), "manual");
    CHECK(ls_queue_add(src, 0, &m, id, sizeof(id), iobuf, err, sizeof(err)) == 0, "queue add: %s", err);
    CHECK(file_exists(src), "copy mode keeps source");
    CHECK(strchr(id, '.') == NULL && strchr(id, '/') == NULL && strchr(id, ' ') == NULL, "id is safe: %s", id);
    ls_queue_row_t r;
    CHECK(ls_queue_get(id, &r) == 1, "row exists");
    char qpath[700]; snprintf(qpath, sizeof(qpath), "%s/%s", LS_QUEUE_DIR, r.filename);
    CHECK(file_exists(qpath), "queue file exists");
    char hex[65]; int64_t sz;
    ls_sha256_file(qpath, hex, &sz, iobuf);
    CHECK(strcmp(hex, r.sha256) == 0 && sz == r.size, "row sha/size match file");
    CHECK(ls_queue_ack(id, "pc", "0000000000000000000000000000000000000000000000000000000000000000") == -2, "wrong sha rejected");
    CHECK(ls_queue_get(id, &r) && r.lan_ack_at == 0, "not acked after mismatch");
    CHECK(ls_queue_ack("nope", "pc", hex) == -1, "unknown id");
    CHECK(ls_queue_ack(id, "pc", hex) == 0, "ack ok");
    CHECK(ls_queue_get(id, &r) && r.lan_ack_at > 0, "acked");
    CHECK(ev_count("PC_ACK") == 1, "PC_ACK logged once");
    unlink(src);
}

static void test_prune(char *iobuf) {
    ls_config_t cfg; ls_config_defaults(&cfg);
    char src[512], err[256], unacked_id[200], acked_id[200], cloud_id[200], hex[65];
    int64_t sz;
    snprintf(src, sizeof(src), "%s/p.zip", g_base);
    ls_queue_meta_t m; memset(&m, 0, sizeof(m));
    snprintf(m.title_id, sizeof(m.title_id), "PPSA00001");
    write_file(src, "unacked");
    ls_queue_add(src, 0, &m, unacked_id, sizeof(unacked_id), iobuf, err, sizeof(err));
    write_file(src, "acked-old");
    ls_queue_add(src, 0, &m, acked_id, sizeof(acked_id), iobuf, err, sizeof(err));
    write_file(src, "acked-cloud-pending");
    ls_queue_add(src, 0, &m, cloud_id, sizeof(cloud_id), iobuf, err, sizeof(err));
    ls_queue_set_cloud_needed(cloud_id);

    ls_sha256_file(src, hex, &sz, iobuf);
    ls_queue_ack(cloud_id, "pc", hex);
    ls_queue_row_t r; ls_queue_get(acked_id, &r);
    ls_queue_ack(acked_id, "pc", r.sha256);
    /* age the acks past keep_acked_hours */
    pthread_mutex_lock(&g_ls_mtx);
    ls_exec_locked("UPDATE queue SET lan_ack_at = lan_ack_at - 1000000 WHERE lan_ack_at > 0");
    pthread_mutex_unlock(&g_ls_mtx);

    ls_queue_prune(&cfg);
    CHECK(ls_queue_get(acked_id, &r) && strcmp(r.state, "pruned") == 0, "expired acked pruned");
    CHECK(ls_queue_get(cloud_id, &r) && strcmp(r.state, "ready") == 0, "cloud-pending kept when under cap");
    CHECK(ls_queue_get(unacked_id, &r) && strcmp(r.state, "ready") == 0, "unacked kept");

    /* "PC and VPS" with the PC uploading: the PC's ack alone is not enough, the vault must have it too. */
    {
        char relay_id[200];
        write_file(src, "relay");
        ls_queue_add(src, 0, &m, relay_id, sizeof(relay_id), iobuf, err, sizeof(err));
        ls_queue_get(relay_id, &r);
        ls_queue_ack(relay_id, "pc", r.sha256);
        pthread_mutex_lock(&g_ls_mtx);
        ls_exec_locked("UPDATE queue SET lan_ack_at = lan_ack_at - 1000000 WHERE lan_ack_at > 0");
        pthread_mutex_unlock(&g_ls_mtx);
        cfg.vps_via_pc = 1;
        ls_queue_prune(&cfg);
        CHECK(ls_queue_get(relay_id, &r) && strcmp(r.state, "ready") == 0, "PC route: kept until the vault has it");
        pthread_mutex_lock(&g_ls_mtx);
        ls_exec_locked("UPDATE queue SET vault_ack_at = 1 WHERE state = 'ready' AND lan_ack_at > 0");
        pthread_mutex_unlock(&g_ls_mtx);
        ls_queue_prune(&cfg);
        CHECK(ls_queue_get(relay_id, &r) && strcmp(r.state, "pruned") == 0 && r.vault_ack_at == 1, "PC route: pruned once both have it");
        cfg.vps_via_pc = 0;
    }

    /* Over cap: unacknowledged must still survive; warning raised. */
    cfg.max_queue_mb = 0; /* below clamp on purpose: prune uses value as given */
    ls_queue_prune(&cfg);
    CHECK(ls_queue_get(unacked_id, &r) && strcmp(r.state, "ready") == 0, "unacked survives over cap");
    char p[700]; snprintf(p, sizeof(p), "%s/%s", LS_QUEUE_DIR, r.filename);
    CHECK(file_exists(p), "unacked file still on disk");
    CHECK(ls_queue_get(cloud_id, &r) && strcmp(r.state, "pruned") == 0, "cloud-pending acked pruned under pressure");
    CHECK(ev_count("QUEUE_CAPACITY_WARNING") >= 1, "capacity warning logged");
    unlink(src);
}

static void test_startup_recovery(char *iobuf) {
    /* Simulate a crash: stale tmp files, orphan stage ZIP, orphan queue ZIP,
       a running op, and a ready row whose file vanished. */
    char p[700];
    snprintf(p, sizeof(p), "%s/half.zip.tmp", LS_QUEUE_DIR); write_file(p, "partial");
    snprintf(p, sizeof(p), "%s/20260101-000000_PPSA09999_all_enc.zip.tmp", g_tmp_dir); write_file(p, "partial");
    snprintf(p, sizeof(p), "%s/20260101-000000_PPSA07777_all_enc.zip", g_tmp_dir); write_file(p, "complete-stage");
    snprintf(p, sizeof(p), "%s/20260101-000000_PPSA05555_s_0001.zip", LS_QUEUE_DIR); write_file(p, "orphan-queue");
    ls_op_begin("auto_backup", "x");

    char src[512], err[256], gone_id[200]; ls_queue_row_t r;
    snprintf(src, sizeof(src), "%s/g.zip", g_base); write_file(src, "will-vanish");
    ls_queue_meta_t m; memset(&m, 0, sizeof(m));
    ls_queue_add(src, 0, &m, gone_id, sizeof(gone_id), iobuf, err, sizeof(err));
    ls_queue_get(gone_id, &r);
    snprintf(p, sizeof(p), "%s/%s", LS_QUEUE_DIR, r.filename); unlink(p);

    int64_t ready_before = q_count("state='ready'");
    ls_startup_recover(iobuf);

    snprintf(p, sizeof(p), "%s/half.zip.tmp", LS_QUEUE_DIR);
    CHECK(!file_exists(p), "queue .tmp removed");
    snprintf(p, sizeof(p), "%s/20260101-000000_PPSA09999_all_enc.zip.tmp", g_tmp_dir);
    CHECK(!file_exists(p), "stage .tmp removed");
    snprintf(p, sizeof(p), "%s/20260101-000000_PPSA07777_all_enc.zip", g_tmp_dir);
    CHECK(!file_exists(p), "stage zip moved into queue");
    CHECK(q_count("reason='recovered_stage' AND title_id='PPSA07777' AND state='ready'") == 1, "stage zip adopted with title");
    CHECK(q_count("filename='20260101-000000_PPSA05555_s_0001.zip' AND state='ready'") == 1, "orphan queue zip adopted");
    CHECK(ls_queue_get(gone_id, &r) && strcmp(r.state, "missing") == 0, "vanished file flagged missing");
    CHECK(q_count("state='ready'") == ready_before - 1 + 2, "ready count adjusted");
    CHECK(ev_count("INTERRUPTED_OPERATION_FOUND") == 1, "interrupted op reported");
    CHECK(ev_count("STALE_TMP_REMOVED") == 2, "two tmp removals logged");

    /* Second restart is idempotent: nothing new adopted, nothing deleted. */
    int64_t ready_after = q_count("state='ready'");
    ls_startup_recover(iobuf);
    CHECK(q_count("state='ready'") == ready_after, "recovery idempotent");
    unlink(src);
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    char *iobuf = malloc(BUF_SIZE);
    CHECK(host_open_state_db(), "db open");
    test_sha256(iobuf);
    test_queue_and_ack(iobuf);
    test_prune(iobuf);
    test_startup_recovery(iobuf);
    free(iobuf);
    return host_finish();
}
