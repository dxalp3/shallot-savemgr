/*
 * Host tests for src/lansync_save.inc.c: enumeration, metadata fingerprints,
 * title signatures and the stability detector (with a scripted fake clock).
 */
#include "host_stubs.h"

#define PS5_DIR LS_USER_HOME "/10000000/savedata_prospero/PPSA01234"
#define PS4_DIR LS_USER_HOME "/10000000/savedata/CUSA05555"
#define DB_DIR  LS_SYSTEM_DATA "/savedata_prospero/10000000/db/user"

static void touch_later(const char *path, const char *data) {
    /* Guarantee a different mtime even on coarse filesystems. */
    write_file(path, data);
    struct timespec ts[2];
    clock_gettime(CLOCK_REALTIME, &ts[0]);
    ts[0].tv_sec += 5;
    ts[1] = ts[0];
    utimensat(AT_FDCWD, path, ts, 0);
}

static void test_enumeration(void) {
    mkdirs(PS5_DIR);
    mkdirs(PS4_DIR);
    write_file(PS5_DIR "/SAVE0001", "ps5-a");
    write_file(PS5_DIR "/SAVE0000", "ps5-b");
    write_file(PS5_DIR "/.hidden", "x");
    write_file(PS4_DIR "/sdimg_SAVEDATA00", "ps4");
    write_file(PS4_DIR "/SAVEDATA00.bin", "key");
    write_file(PS4_DIR "/not_an_image", "x");
    write_file(PS5_DIR "/sdimg_0000.bin", "digimon-style");   /* PS5 save named *.bin */

    ls_save_list_t l = {0};
    ls_enum_saves(&l, "PPSA01234");
    CHECK(l.count == 3, "PPSA01234 saves incl. sdimg_*.bin: %d", l.count);
    CHECK(l.count == 3 && strcmp(l.items[0].save_name, "SAVE0000") == 0, "sorted");
    int found_bin = 0;
    for (int i = 0; i < l.count; i++) if (strcmp(l.items[i].save_name, "sdimg_0000.bin") == 0) found_bin = 1;
    CHECK(found_bin, "sdimg_0000.bin is a save, not a key companion");
    ls_save_list_free(&l);

    ls_enum_saves(&l, NULL);
    CHECK(l.count == 4, "all saves: %d", l.count);
    int found_ps4 = 0;
    for (int i = 0; i < l.count; i++)
        if (strcmp(l.items[i].save_name, "sdimg_SAVEDATA00") == 0 && l.items[i].is_ps4) found_ps4 = 1;
    CHECK(found_ps4, "PS4 sdimg found, .bin and non-image skipped");
    ls_save_list_free(&l);

    ls_enum_saves(&l, "NOPE00000");
    CHECK(l.count == 0, "unknown title has no saves");
    ls_save_list_free(&l);
}

static void test_meta_and_signature(void) {
    char a[65], b[65];
    int n = 0;
    CHECK(ls_title_signature("PPSA01234", a, &n) == 0 && n == 3, "signature");
    ls_title_signature("PPSA01234", b, NULL);
    CHECK(strcmp(a, b) == 0, "unchanged saves give identical signature");

    touch_later(PS5_DIR "/SAVE0001", "ps5-a-modified");
    ls_title_signature("PPSA01234", b, NULL);
    CHECK(strcmp(a, b) != 0, "content write changes signature");

    ls_title_signature("PPSA01234", a, NULL);
    write_file(PS5_DIR "/SAVE0002", "new");
    ls_title_signature("PPSA01234", b, &n);
    CHECK(strcmp(a, b) != 0 && n == 4, "new save changes signature");

    /* PS4 .bin companion is part of the fingerprint. */
    ls_save_list_t l = {0};
    ls_enum_saves(&l, "CUSA05555");
    ls_save_meta_t m1, m2;
    ls_save_meta_read(&l.items[0], NULL, &m1);
    CHECK(m1.exists && m1.bin_size == 3, "bin companion read");
    touch_later(PS4_DIR "/SAVEDATA00.bin", "key2");
    ls_save_meta_read(&l.items[0], NULL, &m2);
    CHECK(!ls_save_meta_equal(&m1, &m2), ".bin change detected");
    ls_save_list_free(&l);

    /* savedata.db mtime is picked up and participates in equality. */
    mkdirs(DB_DIR);
    sqlite3 *db = NULL;
    sqlite3_open(DB_DIR "/savedata.db", &db);
    sqlite3_exec(db, "CREATE TABLE savedata(title_id TEXT, dir_name TEXT, mtime TEXT);"
                     "INSERT INTO savedata VALUES('PPSA01234','SAVE0000','2026-10-07 10:00:00');",
                 NULL, NULL, NULL);
    sqlite3_close(db);
    ls_enum_saves(&l, "PPSA01234");
    ls_save_meta_t *metas = calloc((size_t)l.count, sizeof(*metas));
    ls_read_metas(&l, metas);
    CHECK(strcmp(metas[0].db_mtime, "2026-10-07 10:00:00") == 0, "db mtime read: '%s'", metas[0].db_mtime);
    ls_title_signature("PPSA01234", a, NULL);
    sqlite3_open(DB_DIR "/savedata.db", &db);
    sqlite3_exec(db, "UPDATE savedata SET mtime='2026-10-07 10:05:00'", NULL, NULL, NULL);
    sqlite3_close(db);
    ls_title_signature("PPSA01234", b, NULL);
    CHECK(strcmp(a, b) != 0, "db-only mtime change detected");
    char s1[256];
    ls_save_meta_str(&metas[0], s1, sizeof(s1));
    CHECK(strstr(s1, "db=2026-10-07 10:00:00") != NULL, "meta string: %s", s1);
    free(metas);
    ls_save_list_free(&l);
}

/* ── Scripted stability scenarios ───────────────────────────────── */
typedef struct {
    const char **sigs;      /* sample sequence; last one repeats */
    int nsigs, idx;
    int64_t now;
    int abort_at_sample;    /* abort before this sample index (0 = never) */
} fake_t;

static int fake_sample(void *ctx, char sig[65]) {
    fake_t *f = ctx;
    int i = f->idx < f->nsigs ? f->idx : f->nsigs - 1;
    snprintf(sig, 65, "%s", f->sigs[i]);
    f->idx++;
    return 0;
}
static int fake_abort(void *ctx) { fake_t *f = ctx; return f->abort_at_sample && f->idx >= f->abort_at_sample; }
static void fake_sleep(void *ctx, int s) { fake_t *f = ctx; f->now += (int64_t)s * 1000; }
static int64_t fake_now(void *ctx) { fake_t *f = ctx; return f->now; }

static int run_fake(const char **sigs, int n, int interval, int required, int timeout,
                    int abort_at, ls_stability_result_t *res) {
    fake_t f = { sigs, n, 0, 0, abort_at };
    ls_stability_io_t io = { fake_sample, fake_abort, fake_sleep, fake_now, &f };
    return ls_wait_stable(&io, "TEST00001", interval, required, timeout, res);
}

static void test_stability(void) {
    ls_stability_result_t r;
    const char *fast[] = { "A" };
    CHECK(run_fake(fast, 1, 2, 2, 45, 0, &r) == LS_STABLE, "fast save stable");
    CHECK(r.samples_taken == 3 && r.waited_ms == 4000, "A,B,C over 4s (got %d samples, %lld ms)",
          r.samples_taken, (long long)r.waited_ms);
    CHECK(strcmp(r.final_sig, "A") == 0, "final sig");

    const char *slow[] = { "A", "B", "C", "D", "D" };
    CHECK(run_fake(slow, 5, 2, 2, 45, 0, &r) == LS_STABLE, "slow save eventually stable");
    CHECK(r.changes_seen == 3 && r.samples_taken == 6 && r.waited_ms == 10000,
          "slow: %d changes, %d samples, %lld ms", r.changes_seen, r.samples_taken, (long long)r.waited_ms);

    /* A change after one match resets the count. */
    const char *flap[] = { "A", "A", "B", "B", "B" };
    CHECK(run_fake(flap, 5, 2, 2, 45, 0, &r) == LS_STABLE && r.samples_taken == 5, "match count resets on change");

    const char *busy[] = { "1","2","3","4","5","6","7","8","9","10","11","12","13","14","15","16","17","18","19","20","21","22","23","24","25" };
    CHECK(run_fake(busy, 25, 2, 2, 30, 0, &r) == LS_STABILITY_TIMEOUT, "never-stable times out");
    CHECK(r.waited_ms == 30000, "timeout at 30s (%lld)", (long long)r.waited_ms);

    CHECK(run_fake(fast, 1, 2, 2, 45, 2, &r) == LS_STABILITY_ABORTED, "title start aborts");

    CHECK(run_fake(fast, 1, 2, 1, 45, 0, &r) == LS_STABLE && r.samples_taken == 2, "required=1 needs A==B");

    CHECK(ev_count_code("SAVE_STABILITY_TIMEOUT") == 1, "timeout logged once");
    CHECK(ev_count_code("SAVE_STILL_CHANGING") == 3, "still-changing logged once per run (%lld)",
          (long long)ev_count_code("SAVE_STILL_CHANGING"));
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    test_enumeration();
    test_meta_and_signature();
    test_stability();
    return host_finish();
}
