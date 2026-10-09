/*
 * Host tests for logical save objects (src/lansync_save.inc.c): a primary
 * sdimg_<X> and its system backup copy sdimg_sce_bu_<X> form one object;
 * ids are derived generically; orphans and per-object fingerprints.
 */
#include "host_stubs.h"

#define U    "10000000"
#define BASE LS_USER_HOME "/" U "/savedata_prospero"

static void touch_later(const char *path, const char *data) {
    write_file(path, data);
    struct timespec ts[2];
    clock_gettime(CLOCK_REALTIME, &ts[0]);
    ts[0].tv_sec += 7;
    ts[1] = ts[0];
    utimensat(AT_FDCWD, path, ts, 0);
}

static const ls_object_t *find_obj(const ls_object_list_t *o, const char *id) {
    for (int i = 0; i < o->count; i++) if (strcmp(o->items[i].id, id) == 0) return &o->items[i];
    return NULL;
}

static const char *member(const ls_save_list_t *l, const ls_object_t *o, int role) {
    return o && o->member[role] >= 0 ? l->items[o->member[role]].save_name : "";
}

static void test_naming(void) {
    char id[256];
    CHECK(ls_object_of("sdimg_Save04", id, sizeof(id)) == LS_ROLE_PRIMARY && strcmp(id, "Save04") == 0, "primary id");
    CHECK(ls_object_of("sdimg_sce_bu_Save04", id, sizeof(id)) == LS_ROLE_BACKUP && strcmp(id, "Save04") == 0, "backup id");
    CHECK(ls_object_of("sdimg_0000.bin", id, sizeof(id)) == LS_ROLE_PRIMARY && strcmp(id, "0000.bin") == 0,
          "extension kept in id");
    CHECK(ls_object_of("sdimg_sce_bu_0000.bin", id, sizeof(id)) == LS_ROLE_BACKUP && strcmp(id, "0000.bin") == 0,
          "extension kept in backup id");
    CHECK(ls_object_of("sdimg_sce_bu_", id, sizeof(id)) == LS_ROLE_PRIMARY && strcmp(id, "sce_bu_") == 0,
          "bare prefix is not a backup copy");
    CHECK(ls_object_of("SAVE0000", id, sizeof(id)) == LS_ROLE_PRIMARY && strcmp(id, "SAVE0000") == 0,
          "name without sdimg_ prefix");
    char n[256];
    ls_object_member_name("sdimg_Save04", "Save04", LS_ROLE_BACKUP, n, sizeof(n));
    CHECK(strcmp(n, "sdimg_sce_bu_Save04") == 0, "member name (%s)", n);
}

static void test_grouping(void) {
    /* 1. single object */
    mkdirs(BASE "/PPSA00001");
    write_file(BASE "/PPSA00001/sdimg_system", "a");
    write_file(BASE "/PPSA00001/sdimg_sce_bu_system", "a");
    /* 2. multi-slot + 4. primary without backup */
    mkdirs(BASE "/PPSA00002");
    write_file(BASE "/PPSA00002/sdimg_Save00", "0");
    write_file(BASE "/PPSA00002/sdimg_sce_bu_Save00", "0");
    write_file(BASE "/PPSA00002/sdimg_Save04", "4");
    write_file(BASE "/PPSA00002/sdimg_sce_bu_Save04", "4");
    write_file(BASE "/PPSA00002/sdimg_Save05", "5");
    /* 3. extension-preserving names + 5. orphan backup copy */
    mkdirs(BASE "/PPSA00003");
    write_file(BASE "/PPSA00003/sdimg_0000.bin", "d");
    write_file(BASE "/PPSA00003/sdimg_sce_bu_0000.bin", "d");
    write_file(BASE "/PPSA00003/sdimg_sce_bu_0001.bin", "orphan");

    ls_save_list_t l = {0};
    ls_object_list_t o = {0};

    ls_enum_saves(&l, "PPSA00001");
    CHECK(ls_group_objects(&l, &o) == 0 && o.count == 1, "single object (%d)", o.count);
    const ls_object_t *x = find_obj(&o, "system");
    CHECK(x && strcmp(member(&l, x, 0), "sdimg_system") == 0 && strcmp(member(&l, x, 1), "sdimg_sce_bu_system") == 0,
          "system: primary + backup");
    ls_object_list_free(&o); ls_save_list_free(&l);

    ls_enum_saves(&l, "PPSA00002");
    ls_group_objects(&l, &o);
    CHECK(l.count == 5 && o.count == 3, "5 files -> 3 objects (%d/%d)", l.count, o.count);
    x = find_obj(&o, "Save04");
    CHECK(x && strcmp(member(&l, x, 0), "sdimg_Save04") == 0 && strcmp(member(&l, x, 1), "sdimg_sce_bu_Save04") == 0,
          "Save04 pair");
    x = find_obj(&o, "Save05");
    CHECK(x && x->member[1] < 0 && !ls_object_is_orphan(x), "Save05 primary without backup is valid");
    CHECK(!find_obj(&o, "sce_bu_Save00"), "backup copy is not its own object");
    ls_object_list_free(&o); ls_save_list_free(&l);

    ls_enum_saves(&l, "PPSA00003");
    ls_group_objects(&l, &o);
    CHECK(o.count == 2, "0000.bin + orphan 0001.bin (%d)", o.count);
    x = find_obj(&o, "0000.bin");
    CHECK(x && x->member[0] >= 0 && x->member[1] >= 0, "0000.bin pair");
    x = find_obj(&o, "0001.bin");
    CHECK(x && ls_object_is_orphan(x) && strcmp(member(&l, x, 1), "sdimg_sce_bu_0001.bin") == 0, "orphan kept as object");
    ls_object_list_free(&o); ls_save_list_free(&l);

    /* Whole catalogue: objects never merge across titles. */
    ls_enum_saves(&l, NULL);
    ls_group_objects(&l, &o);
    CHECK(o.count == 6, "catalogue objects (%d)", o.count);
    ls_object_list_free(&o); ls_save_list_free(&l);
}

static void fingerprints(const char *tid, const char *ids[], char fps[][65], int n) {
    ls_save_list_t l = {0};
    ls_object_list_t o = {0};
    ls_enum_saves(&l, tid);
    ls_save_meta_t *m = calloc((size_t)l.count, sizeof(*m));
    ls_read_metas(&l, m);
    ls_group_objects(&l, &o);
    for (int k = 0; k < n; k++) {
        fps[k][0] = 0;
        const ls_object_t *x = find_obj(&o, ids[k]);
        if (x) ls_object_fingerprint(&l, m, x, fps[k]);
    }
    free(m);
    ls_object_list_free(&o);
    ls_save_list_free(&l);
}

/* 9. per-object change detection */
static void test_fingerprints(void) {
    const char *ids[] = { "Save00", "Save04", "Save05" };
    char a[3][65], b[3][65];
    fingerprints("PPSA00002", ids, a, 3);
    fingerprints("PPSA00002", ids, b, 3);
    CHECK(memcmp(a, b, sizeof(a)) == 0, "stable fingerprints");
    touch_later(BASE "/PPSA00002/sdimg_sce_bu_Save04", "4-new");
    fingerprints("PPSA00002", ids, b, 3);
    CHECK(strcmp(a[0], b[0]) == 0 && strcmp(a[2], b[2]) == 0, "Save00/Save05 unchanged");
    CHECK(strcmp(a[1], b[1]) != 0, "backup-copy change shows on Save04");
    fingerprints("PPSA00002", ids, a, 3);
    unlink(BASE "/PPSA00002/sdimg_sce_bu_Save04");
    fingerprints("PPSA00002", ids, b, 3);
    CHECK(strcmp(a[1], b[1]) != 0 && strcmp(a[0], b[0]) == 0, "backup copy disappearing changes only Save04");
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    test_naming();
    test_grouping();
    test_fingerprints();
    return host_finish();
}
